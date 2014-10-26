/*
 * Elantech Touchpad driver (v6)
 *
 * Copyright (C) 2007-2009 Arjan Opmeer <arjan@opmeer.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * Trademarks are the property of their respective owners.
 */

#define pr_fmt(fmt) KBUILD_BASENAME ": " fmt

#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/serio.h>
#include <linux/libps2.h>
#include "psmouse.h"
#include "elantech.h"

#define FW_VERSION_MAJOR_MASK	0xff0000
#define FW_VERSION_MINOR_MASK	0x00ff00
#define FW_VERSION_MICRO_MASK	0x0000ff

#define elantech_debug(fmt, ...)					\
	do {								\
		if (etd->debug)						\
			printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__);	\
	} while (0)

static bool force_elantech;
module_param_named(force_elantech, force_elantech, bool, 0644);
MODULE_PARM_DESC(force_elantech, "Force the Elantech PS/2 protocol extension to be used, 1 = enabled, 0 = disabled (default).");

/*
 * Send a Synaptics style sliced query command
 */
static int synaptics_send_cmd(struct psmouse *psmouse, unsigned char c,
				unsigned char *param)
{
	if (psmouse_sliced_command(psmouse, c) ||
	    ps2_command(&psmouse->ps2dev, param, PSMOUSE_CMD_GETINFO)) {
		pr_err("synaptics_send_cmd query 0x%02x failed.\n", c);
		return -1;
	}

	return 0;
}

/*
 * A retrying version of ps2_command
 */
static int elantech_ps2_command(struct psmouse *psmouse,
				unsigned char *param, int command)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	struct elantech_data *etd = psmouse->private;
	int rc;
	int tries = ETP_PS2_COMMAND_TRIES;

	do {
		rc = ps2_command(ps2dev, param, command);
		if (rc == 0)
			break;
		tries--;
		elantech_debug("retrying ps2 command 0x%02x (%d).\n",
				command, tries);
		msleep(ETP_PS2_COMMAND_DELAY);
	} while (tries > 0);

	if (rc)
		pr_err("ps2 command 0x%02x failed.\n", command);

	return rc;
}

/*
 * Send an Elantech style special command to read a value from a register
 */
static int elantech_read_reg(struct psmouse *psmouse, unsigned char reg,
				unsigned char *val)
{
	struct elantech_data *etd = psmouse->private;
	unsigned char param[3];
	unsigned char command;
	int rc = 0;

	if (reg < 0x10 || reg > 0x26)
		return -1;

	if (reg > 0x11 && reg < 0x20)
		return -1;

	command = (etd->hw_version == 3) ? ETP_REGISTER_RW : ETP_REGISTER_READ;

	switch (etd->hw_version) {
	case 1:
		if (psmouse_sliced_command(psmouse, command) ||
		    psmouse_sliced_command(psmouse, reg) ||
		    ps2_command(&psmouse->ps2dev, param, PSMOUSE_CMD_GETINFO)) {
			rc = -1;
		}
		break;

	case 2:
	case 3:
		if (elantech_ps2_command(psmouse,  NULL, ETP_PS2_CUSTOM_COMMAND) ||
		    elantech_ps2_command(psmouse,  NULL, command) ||
		    elantech_ps2_command(psmouse,  NULL, ETP_PS2_CUSTOM_COMMAND) ||
		    elantech_ps2_command(psmouse,  NULL, reg) ||
		    elantech_ps2_command(psmouse, param, PSMOUSE_CMD_GETINFO)) {
			rc = -1;
		}
		break;
	}

	if (rc)
		pr_err("failed to read register 0x%02x.\n", reg);
	else
		*val = param[0];

	return rc;
}

/*
 * Send an Elantech style special command to write a register with a value
 */
static int elantech_write_reg(struct psmouse *psmouse, unsigned char reg,
				unsigned char val)
{
	struct elantech_data *etd = psmouse->private;
	unsigned char command;
	int rc = 0;

	if (reg < 0x10 || reg > 0x26)
		return -1;

	if (reg > 0x11 && reg < 0x20)
		return -1;

	command = (etd->hw_version == 3) ? ETP_REGISTER_RW : ETP_REGISTER_WRITE;

	switch (etd->hw_version) {
	case 1:
		if (psmouse_sliced_command(psmouse, command) ||
		    psmouse_sliced_command(psmouse, reg) ||
		    psmouse_sliced_command(psmouse, val) ||
		    ps2_command(&psmouse->ps2dev, NULL, PSMOUSE_CMD_SETSCALE11)) {
			rc = -1;
		}
		break;

	case 2:
		if (elantech_ps2_command(psmouse, NULL, ETP_PS2_CUSTOM_COMMAND) ||
		    elantech_ps2_command(psmouse, NULL, command) ||
		    elantech_ps2_command(psmouse, NULL, ETP_PS2_CUSTOM_COMMAND) ||
		    elantech_ps2_command(psmouse, NULL, reg) ||
		    elantech_ps2_command(psmouse, NULL, ETP_PS2_CUSTOM_COMMAND) ||
		    elantech_ps2_command(psmouse, NULL, val) ||
		    elantech_ps2_command(psmouse, NULL, PSMOUSE_CMD_SETSCALE11)) {
			rc = -1;
		}
		break;
	}

	if (rc)
		pr_err("failed to write register 0x%02x with value 0x%02x.\n",
			reg, val);

	return rc;
}

/*
 * Set the elantech touchpad mode byte via special commands
 */
static int elantech_mode_command(struct psmouse *psmouse, unsigned char reg,
				unsigned char val)
{
	struct elantech_data *etd = psmouse->private;
	unsigned char param[3];
	int write_cmd, read_cmd;

	if (etd->hw_version == 3) {
		write_cmd = ETP_REGISTER_RW;
		read_cmd = ETP_REGISTER_RW;
	} else {
		write_cmd = ETP_REGISTER_READ;
		read_cmd = ETP_REGISTER_WRITE;
	}

	if (psmouse_sliced_command(psmouse, write_cmd) ||
	    psmouse_sliced_command(psmouse, reg) ||
	    psmouse_sliced_command(psmouse, val) ||
	    ps2_command(&psmouse->ps2dev, NULL, PSMOUSE_CMD_SETSCALE11))
		return -1;

	if (psmouse_sliced_command(psmouse, read_cmd) ||
	    psmouse_sliced_command(psmouse, reg) ||
	    ps2_command(&psmouse->ps2dev, param, PSMOUSE_CMD_GETINFO))
		return -1;

	if (val != param[0])
		return -1;

	return 0;
}

/*
 * Dump a complete mouse movement packet to the syslog
 */
static void elantech_packet_dump(unsigned char *packet, int size)
{
	int	i;

	printk(KERN_DEBUG pr_fmt("PS/2 packet ["));
	for (i = 0; i < size; i++)
		printk("%s0x%02x ", (i) ? ", " : " ", packet[i]);
	printk("]\n");
}

/*
 * Interpret complete data packets and report absolute mode input events for
 * hardware version 1. (4 byte packets)
 */
static void elantech_report_absolute_v1(struct psmouse *psmouse)
{
	struct input_dev *dev = psmouse->dev;
	struct elantech_data *etd = psmouse->private;
	unsigned char *packet = psmouse->packet;
	int fingers;

	if (etd->fw_version < 0x020000) {
		/*
		 * byte 0:  D   U  p1  p2   1  p3   R   L
		 * byte 1:  f   0  th  tw  x9  x8  y9  y8
		 */
		fingers = ((packet[1] & 0x80) >> 7) +
				((packet[1] & 0x30) >> 4);
	} else {
		/*
		 * byte 0: n1  n0  p2  p1   1  p3   R   L
		 * byte 1:  0   0   0   0  x9  x8  y9  y8
		 */
		fingers = (packet[0] & 0xc0) >> 6;
	}

	if (etd->jumpy_cursor) {
		if (fingers != 1) {
			etd->single_finger_reports = 0;
		} else if (etd->single_finger_reports < 2) {
			/* Discard first 2 reports of one finger, bogus */
			etd->single_finger_reports++;
			elantech_debug("discarding packet\n");
			return;
		}
	}

	input_report_key(dev, BTN_TOUCH, fingers != 0);

	/*
	 * byte 2: x7  x6  x5  x4  x3  x2  x1  x0
	 * byte 3: y7  y6  y5  y4  y3  y2  y1  y0
	 */
	if (fingers) {
		input_report_abs(dev, ABS_X,
			((packet[1] & 0x0c) << 6) | packet[2]);
		input_report_abs(dev, ABS_Y,
			etd->y_max - (((packet[1] & 0x03) << 8) | packet[3]));
	}

	input_report_key(dev, BTN_TOOL_FINGER, fingers == 1);
	input_report_key(dev, BTN_TOOL_DOUBLETAP, fingers == 2);
	input_report_key(dev, BTN_TOOL_TRIPLETAP, fingers == 3);
	input_report_key(dev, BTN_LEFT, packet[0] & 0x01);
	input_report_key(dev, BTN_RIGHT, packet[0] & 0x02);

	if (etd->fw_version < 0x020000 &&
	    (etd->capabilities & ETP_CAP_HAS_ROCKER)) {
		/* rocker up */
		input_report_key(dev, BTN_FORWARD, packet[0] & 0x40);
		/* rocker down */
		input_report_key(dev, BTN_BACK, packet[0] & 0x80);
	}

	input_sync(dev);
}

static void elantech_set_slot(struct input_dev *dev, int slot, bool active,
			      unsigned int x, unsigned int y)
{
	input_mt_slot(dev, slot);
	input_mt_report_slot_state(dev, MT_TOOL_FINGER, active);
	if (active) {
		input_report_abs(dev, ABS_MT_POSITION_X, x);
		input_report_abs(dev, ABS_MT_POSITION_Y, y);
	}
}

/* x1 < x2 and y1 < y2 when two fingers, x = y = 0 when not pressed */
static void elantech_report_semi_mt_data(struct input_dev *dev,
					 unsigned int num_fingers,
					 unsigned int x1, unsigned int y1,
					 unsigned int x2, unsigned int y2)
{
	elantech_set_slot(dev, 0, num_fingers != 0, x1, y1);
	elantech_set_slot(dev, 1, num_fingers == 2, x2, y2);
}

/*
 * Interpret complete data packets and report absolute mode input events for
 * hardware version 2. (6 byte packets)
 */
static void elantech_report_absolute_v2(struct psmouse *psmouse)
{
	struct elantech_data *etd = psmouse->private;
	struct input_dev *dev = psmouse->dev;
	unsigned char *packet = psmouse->packet;
	unsigned int fingers, x1 = 0, y1 = 0, x2 = 0, y2 = 0, width = 0, pres = 0;

	/* byte 0: n1  n0   .   .   .   .   R   L */
	fingers = (packet[0] & 0xc0) >> 6;
	input_report_key(dev, BTN_TOUCH, fingers != 0);

	switch (fingers) {
	case 3:
		/*
		 * Same as one finger, except report of more than 3 fingers:
		 * byte 3:  n4  .   w1  w0   .   .   .   .
		 */
		if (packet[3] & 0x80)
			fingers = 4;
		/* pass through... */
	case 1:
		/*
		 * byte 1:  .   .   .   .   .  x10 x9  x8
		 * byte 2: x7  x6  x5  x4  x4  x2  x1  x0
		 */
		x1 = ((packet[1] & 0x07) << 8) | packet[2];
		/*
		 * byte 4:  .   .   .   .   .   .  y9  y8
		 * byte 5: y7  y6  y5  y4  y3  y2  y1  y0
		 */
		y1 = etd->y_max - (((packet[4] & 0x03) << 8) | packet[5]);

		input_report_abs(dev, ABS_X, x1);
		input_report_abs(dev, ABS_Y, y1);

		pres = (packet[1] & 0xf0) | ((packet[4] & 0xf0) >> 4);
		width = ((packet[0] & 0x30) >> 2) | ((packet[3] & 0x30) >> 4);
		break;

	case 2:
		/*
		 * The coordinate of each finger is reported separately
		 * with a lower resolution for two finger touches:
		 * byte 0:  .   .  ay8 ax8  .   .   .   .
		 * byte 1: ax7 ax6 ax5 ax4 ax3 ax2 ax1 ax0
		 */
		x1 = ((packet[0] & 0x10) << 4) | packet[1];
		/* byte 2: ay7 ay6 ay5 ay4 ay3 ay2 ay1 ay0 */
		y1 = etd->y_max_2ft - (((packet[0] & 0x20) << 3) | packet[2]);
		/*
		 * byte 3:  .   .  by8 bx8  .   .   .   .
		 * byte 4: bx7 bx6 bx5 bx4 bx3 bx2 bx1 bx0
		 */
		x2 = ((packet[3] & 0x10) << 4) | packet[4];
		/* byte 5: by7 by8 by5 by4 by3 by2 by1 by0 */
		y2 = etd->y_max_2ft - (((packet[3] & 0x20) << 3) | packet[5]);
		/*
		 * For compatibility with the X Synaptics driver scale up
		 * one coordinate and report as ordinary mouse movent
		 */
		input_report_abs(dev, ABS_X, x1 << 2);
		input_report_abs(dev, ABS_Y, y1 << 2);

		/* Unknown so just report sensible values */
		pres = 127;
		width = 7;
		break;
	}

	elantech_report_semi_mt_data(dev, fingers, x1, y1, x2, y2);
	input_report_key(dev, BTN_TOOL_FINGER, fingers == 1);
	input_report_key(dev, BTN_TOOL_DOUBLETAP, fingers == 2);
	input_report_key(dev, BTN_TOOL_TRIPLETAP, fingers == 3);
	input_report_key(dev, BTN_TOOL_QUADTAP, fingers == 4);
	input_report_key(dev, BTN_LEFT, packet[0] & 0x01);
	input_report_key(dev, BTN_RIGHT, packet[0] & 0x02);
	if (etd->reports_pressure) {
		input_report_abs(dev, ABS_PRESSURE, pres);
		input_report_abs(dev, ABS_TOOL_WIDTH, width);
	}

	input_sync(dev);
}

/*
 * Interpret complete data packets and report absolute mode input events for
 * hardware version 3. (6 byte packets)
 */
static psmouse_ret_t elantech_report_absolute_v3(struct psmouse *psmouse)
{
	struct elantech_data *etd = psmouse->private;
	struct input_dev *dev = psmouse->dev;
	unsigned char *packet1 = psmouse->packet;
	unsigned char *packet2 = NULL;
	unsigned int fingers, width = 0, pres = 0;
	unsigned int x1, y1, x2 = 0, y2 = 0;

	/* byte 0: n1  n0   .   .   .   .   R   L */
	fingers = (packet1[0] & 0xc0) >> 6;

	/*
	 * Need to receive two packets for 2-touch events. If this
	 * is the first packet, store it away and wait for the second.
	 * We don't support parity in v3, so use that storage.
	 */
	if (fingers == 2 && (packet1[0] & 0x0c) == 0x04) {
		memcpy(etd->parity, packet1, psmouse->pktsize);
		return PSMOUSE_FULL_PACKET;
	}

	/*
	 * If this the second packet of a two-fingered touch event we
	 * need to process the previous packet stashed away in
	 * etd->parity
	 */
	if (fingers == 2) {
		packet1 = etd->parity;
		packet2 = psmouse->packet;
	}

	x1 = ((packet1[1] & 0x0f) << 8) | packet1[2];
	y1 = etd->y_max - (((packet1[4] & 0x0f) << 8) | packet1[5]);

	if (packet2) {
		x2 = ((packet2[1] & 0x0f) << 8) | packet2[2];
		y2 = etd->y_max - (((packet2[4] & 0x0f) << 8) | packet2[5]);
	}

	/*
	 * Reject crazy values we sometimes get from the hardware
	 */
	if (fingers != 0 && (x1 > etd->x_max || y1 > etd->y_max))
		return PSMOUSE_FULL_PACKET;

	pres = (packet1[1] & 0xf0) | ((packet1[4] & 0xf0) >> 4);
	width = ((packet1[0] & 0x30) >> 2) | ((packet1[3] & 0x30) >> 4);

	elantech_report_semi_mt_data(dev, fingers, x1, y1, x2, y2);

	input_report_key(dev, BTN_TOUCH, fingers != 0);
	input_report_key(dev, BTN_TOOL_FINGER, fingers == 1);
	input_report_key(dev, BTN_TOOL_DOUBLETAP, fingers == 2);
	input_report_key(dev, BTN_TOOL_TRIPLETAP, fingers == 3);

	if (etd->reports_pressure) {
		input_report_abs(dev, ABS_PRESSURE, pres);
		input_report_abs(dev, ABS_TOOL_WIDTH, width);
	}

	if (fingers != 0) {
		input_report_abs(dev, ABS_X, x1);
		input_report_abs(dev, ABS_Y, y1);
	}

	input_report_key(dev, BTN_LEFT, packet1[0] & 0x01);
	input_report_key(dev, BTN_RIGHT, packet1[0] & 0x02);

	input_sync(dev);
	return PSMOUSE_FULL_PACKET;
}

static int elantech_check_parity_v1(struct psmouse *psmouse)
{
	struct elantech_data *etd = psmouse->private;
	unsigned char *packet = psmouse->packet;
	unsigned char p1, p2, p3;

	/* Parity bits are placed differently */
	if (etd->fw_version < 0x020000) {
		/* byte 0:  D   U  p1  p2   1  p3   R   L */
		p1 = (packet[0] & 0x20) >> 5;
		p2 = (packet[0] & 0x10) >> 4;
	} else {
		/* byte 0: n1  n0  p2  p1   1  p3   R   L */
		p1 = (packet[0] & 0x10) >> 4;
		p2 = (packet[0] & 0x20) >> 5;
	}

	p3 = (packet[0] & 0x04) >> 2;

	return etd->parity[packet[1]] == p1 &&
	       etd->parity[packet[2]] == p2 &&
	       etd->parity[packet[3]] == p3;
}

/*
 * Process byte stream from mouse and handle complete packets
 */
static psmouse_ret_t elantech_process_byte(struct psmouse *psmouse)
{
	struct elantech_data *etd = psmouse->private;

	if (psmouse->pktcnt < psmouse->pktsize)
		return PSMOUSE_GOOD_DATA;

	if (etd->debug > 1)
		elantech_packet_dump(psmouse->packet, psmouse->pktsize);

	switch (etd->hw_version) {
	case 1:
		if (etd->paritycheck && !elantech_check_parity_v1(psmouse))
			return PSMOUSE_BAD_DATA;

		elantech_report_absolute_v1(psmouse);
		break;

	case 2:
		/* We don't know how to check parity in protocol v2 */
		elantech_report_absolute_v2(psmouse);
		break;
	case 3:
		/* We don't know how to check parity in protocol v3 */
		return elantech_report_absolute_v3(psmouse);
	}

	return PSMOUSE_FULL_PACKET;
}

/*
 * Put the touchpad into absolute mode
 */
static int elantech_set_absolute_mode(struct psmouse *psmouse)
{
	struct elantech_data *etd = psmouse->private;
	unsigned char val;
	int tries = ETP_READ_BACK_TRIES;
	int rc = 0;

	switch (etd->hw_version) {
	case 1:
		etd->reg_10 = 0x16;
		etd->reg_11 = 0x8f;
		if (elantech_write_reg(psmouse, 0x10, etd->reg_10) ||
		    elantech_write_reg(psmouse, 0x11, etd->reg_11)) {
			rc = -1;
		}
		break;

	case 2:
					/* Windows driver values */
		etd->reg_10 = 0x54;
		etd->reg_11 = 0x88;	/* 0x8a */
		etd->reg_21 = 0x60;	/* 0x00 */
		if (elantech_write_reg(psmouse, 0x10, etd->reg_10) ||
		    elantech_write_reg(psmouse, 0x11, etd->reg_11) ||
		    elantech_write_reg(psmouse, 0x21, etd->reg_21))
			rc = -1;
		break;
	case 3:
		etd->reg_10 = 0x0f;
		rc = elantech_mode_command(psmouse, 0x10, etd->reg_10);
		break;
	}

	if (etd->hw_version != 3 && rc == 0) {
		/*
		 * Read back reg 0x10. For hardware version 1 we must make
		 * sure the absolute mode bit is set. For hardware version 2
		 * the touchpad is probably initalising and not ready until
		 * we read back the value we just wrote.
		 */
		do {
			rc = elantech_read_reg(psmouse, 0x10, &val);
			if (rc == 0)
				break;
			tries--;
			elantech_debug("retrying read (%d).\n", tries);
			msleep(ETP_READ_BACK_DELAY);
		} while (tries > 0);

		if (rc) {
			pr_err("failed to read back register 0x10.\n");
		} else if (etd->hw_version == 1 &&
			   !(val & ETP_R10_ABSOLUTE_MODE)) {
			pr_err("touchpad refuses to switch to absolute mode.\n");
			rc = -1;
		}
	}

	if (rc)
		pr_err("failed to initialise registers.\n");

	return rc;
}

/*
 * Set the appropriate event bits for the input subsystem
 */
static int elantech_set_input_params(struct psmouse *psmouse)
{
	struct input_dev *dev = psmouse->dev;
	struct elantech_data *etd = psmouse->private;
	unsigned char param[3];

	__set_bit(EV_KEY, dev->evbit);
	__set_bit(EV_ABS, dev->evbit);
	__clear_bit(EV_REL, dev->evbit);

	__set_bit(BTN_LEFT, dev->keybit);
	__set_bit(BTN_RIGHT, dev->keybit);

	__set_bit(BTN_TOUCH, dev->keybit);
	__set_bit(BTN_TOOL_FINGER, dev->keybit);
	__set_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
	__set_bit(BTN_TOOL_TRIPLETAP, dev->keybit);

	switch (etd->hw_version) {
	case 1:
		etd->x_max = ETP_XMAX_V1;
		etd->y_max = ETP_YMAX_V1;

		/* Rocker button */
		if (etd->fw_version < 0x020000 &&
		    (etd->capabilities & ETP_CAP_HAS_ROCKER)) {
			__set_bit(BTN_FORWARD, dev->keybit);
			__set_bit(BTN_BACK, dev->keybit);
		}
		input_set_abs_params(dev, ABS_X, ETP_XMIN_V1, etd->x_max, 0, 0);
		input_set_abs_params(dev, ABS_Y, ETP_YMIN_V1, etd->y_max, 0, 0);
		break;

	case 2:
	case 3:
		if (etd->hw_version == 3) {
			if (ps2_command(&psmouse->ps2dev, NULL, ETP_PS2_CUSTOM_COMMAND) ||
			    ps2_command(&psmouse->ps2dev, NULL, 0x00) ||
			    ps2_command(&psmouse->ps2dev, param, PSMOUSE_CMD_GETINFO))
				return -1;

			etd->x_max = (param[0] & 0xf) << 8 | param[1];
			etd->y_max = (param[0] & 0xf0) << 4 | param[2];
			etd->y_max_2ft = etd->y_max;
			elantech_debug("x_max = %d, y_max = %d\n",
				       etd->x_max, etd->y_max);
		} else {
			etd->x_max = ETP_XMAX_V2;
			etd->y_max = ETP_YMAX_V2;
			etd->y_max_2ft = ETP_2FT_YMAX;
		}

		__set_bit(BTN_TOOL_QUADTAP, dev->keybit);
		input_set_abs_params(dev, ABS_X, ETP_XMIN_V2, etd->x_max, 0, 0);
		input_set_abs_params(dev, ABS_Y, ETP_YMIN_V2, etd->y_max, 0, 0);
		if (etd->reports_pressure) {
			input_set_abs_params(dev, ABS_PRESSURE, ETP_PMIN_V2,
					     ETP_PMAX_V2, 0, 0);
			input_set_abs_params(dev, ABS_TOOL_WIDTH, ETP_WMIN_V2,
					     ETP_WMAX_V2, 0, 0);
		}
		__set_bit(INPUT_PROP_SEMI_MT, dev->propbit);
		input_mt_init_slots(dev, 2);
		input_set_abs_params(dev, ABS_MT_POSITION_X, ETP_XMIN_V2, etd->x_max, 0, 0);
		input_set_abs_params(dev, ABS_MT_POSITION_Y, ETP_YMIN_V2, etd->y_max, 0, 0);
		break;
	}

	return 0;
}

struct elantech_attr_data {
	size_t		field_offset;
	unsigned char	reg;
};

/*
 * Display a register value by reading a sysfs entry
 */
static ssize_t elantech_show_int_attr(struct psmouse *psmouse, void *data,
					char *buf)
{
	struct elantech_data *etd = psmouse->private;
	struct elantech_attr_data *attr = data;
	unsigned char *reg = (unsigned char *) etd + attr->field_offset;
	int rc = 0;

	if (attr->reg)
		rc = elantech_read_reg(psmouse, attr->reg, reg);

	return sprintf(buf, "0x%02x\n", (attr->reg && rc) ? -1 : *reg);
}

/*
 * Write a register value by writing a sysfs entry
 */
static ssize_t elantech_set_int_attr(struct psmouse *psmouse,
				     void *data, const char *buf, size_t count)
{
	struct elantech_data *etd = psmouse->private;
	struct elantech_attr_data *attr = data;
	unsigned char *reg = (unsigned char *) etd + attr->field_offset;
	unsigned long value;
	int err;

	err = strict_strtoul(buf, 16, &value);
	if (err)
		return err;

	if (value > 0xff)
		return -EINVAL;

	/* Do we need to preserve some bits for version 2 hardware too? */
	if (etd->hw_version == 1) {
		if (attr->reg == 0x10)
			/* Force absolute mode always on */
			value |= ETP_R10_ABSOLUTE_MODE;
		else if (attr->reg == 0x11)
			/* Force 4 byte mode always on */
			value |= ETP_R11_4_BYTE_MODE;
	}

	if (!attr->reg || elantech_write_reg(psmouse, attr->reg, value) == 0)
		*reg = value;

	return count;
}

#define ELANTECH_INT_ATTR(_name, _register)				\
	static struct elantech_attr_data elantech_attr_##_name = {	\
		.field_offset = offsetof(struct elantech_data, _name),	\
		.reg = _register,					\
	};								\
	PSMOUSE_DEFINE_ATTR(_name, S_IWUSR | S_IRUGO,			\
			    &elantech_attr_##_name,			\
			    elantech_show_int_attr,			\
			    elantech_set_int_attr)

ELANTECH_INT_ATTR(reg_10, 0x10);
ELANTECH_INT_ATTR(reg_11, 0x11);
ELANTECH_INT_ATTR(reg_20, 0x20);
ELANTECH_INT_ATTR(reg_21, 0x21);
ELANTECH_INT_ATTR(reg_22, 0x22);
ELANTECH_INT_ATTR(reg_23, 0x23);
ELANTECH_INT_ATTR(reg_24, 0x24);
ELANTECH_INT_ATTR(reg_25, 0x25);
ELANTECH_INT_ATTR(reg_26, 0x26);
ELANTECH_INT_ATTR(debug, 0);
ELANTECH_INT_ATTR(paritycheck, 0);

static struct attribute *elantech_attrs[] = {
	&psmouse_attr_reg_10.dattr.attr,
	&psmouse_attr_reg_11.dattr.attr,
	&psmouse_attr_reg_20.dattr.attr,
	&psmouse_attr_reg_21.dattr.attr,
	&psmouse_attr_reg_22.dattr.attr,
	&psmouse_attr_reg_23.dattr.attr,
	&psmouse_attr_reg_24.dattr.attr,
	&psmouse_attr_reg_25.dattr.attr,
	&psmouse_attr_reg_26.dattr.attr,
	&psmouse_attr_debug.dattr.attr,
	&psmouse_attr_paritycheck.dattr.attr,
	NULL
};

static struct attribute_group elantech_attr_group = {
	.attrs = elantech_attrs,
};

static bool elantech_is_signature_valid(const unsigned char *param)
{
	static const unsigned char rates[] = { 200, 100, 80, 60, 40, 20, 10 };
	int i;

	if (param[0] == 0)
		return false;

	if (param[1] == 0)
		return true;

	for (i = 0; i < ARRAY_SIZE(rates); i++)
		if (param[2] == rates[i])
			return false;

	return true;
}

/*
 * Use magic knock to detect Elantech touchpad
 */

static const char elantech_magic_knocks[][3] = {
	{ '\x3c', '\x03', '\xc8' },
	{ '\x3c', '\x03', '\x00' },
};

int elantech_detect(struct psmouse *psmouse, bool set_properties)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	unsigned char param[3];
	int i;

	ps2_command(&psmouse->ps2dev, NULL, PSMOUSE_CMD_RESET_DIS);

	if (ps2_command(ps2dev,  NULL, PSMOUSE_CMD_DISABLE) ||
	    ps2_command(ps2dev,  NULL, PSMOUSE_CMD_SETSCALE11) ||
	    ps2_command(ps2dev,  NULL, PSMOUSE_CMD_SETSCALE11) ||
	    ps2_command(ps2dev,  NULL, PSMOUSE_CMD_SETSCALE11) ||
	    ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO)) {
		pr_debug("sending Elantech magic knock failed.\n");
		return -1;
	}

	for (i = 0; i < ARRAY_SIZE(elantech_magic_knocks); i++) {
		if (!memcmp(param, elantech_magic_knocks[i], sizeof(param)))
			break;
	}
	if (i >= ARRAY_SIZE(elantech_magic_knocks)) {
		/*
		 * Report this in case there are Elantech models that use a
		 * different set of magic numbers
		 */
		pr_debug("unexpected magic knock result 0x%02x, 0x%02x, 0x%02x.\n",
			 param[0], param[1], param[2]);
	}

	/*
	 * Query touchpad's firmware version and see if it reports known
	 * value to avoid mis-detection. Logitech mice are known to respond
	 * to Elantech magic knock and there might be more.
	 */
	if (synaptics_send_cmd(psmouse, ETP_FW_VERSION_QUERY, param)) {
		pr_debug("failed to query firmware version.\n");
		return -1;
	}

	pr_debug("Elantech version query result 0x%02x, 0x%02x, 0x%02x.\n",
		 param[0], param[1], param[2]);

	if (!elantech_is_signature_valid(param)) {
		if (!force_elantech) {
			pr_debug("Probably not a real Elantech touchpad. Aborting.\n");
			return -1;
		}

		pr_debug("Probably not a real Elantech touchpad. Enabling anyway due to force_elantech.\n");
	}

	if (set_properties) {
		psmouse->vendor = "Elantech";
		psmouse->name = "Touchpad";
	}

	return 0;
}

/*
 * Clean up sysfs entries when disconnecting
 */
static void elantech_disconnect(struct psmouse *psmouse)
{
	sysfs_remove_group(&psmouse->ps2dev.serio->dev.kobj,
			   &elantech_attr_group);
	kfree(psmouse->private);
	psmouse->private = NULL;
}

/*
 * Put the touchpad back into absolute mode when reconnecting
 */
static int elantech_reconnect(struct psmouse *psmouse)
{
	if (elantech_detect(psmouse, 0))
		return -1;

	if (elantech_set_absolute_mode(psmouse)) {
		pr_err("failed to put touchpad back into absolute mode.\n");
		return -1;
	}

	return 0;
}

/*
 * Initialize the touchpad and create sysfs entries
 */
int elantech_init(struct psmouse *psmouse)
{
	struct elantech_data *etd;
	int i, error;
	unsigned char param[3];

	psmouse->private = etd = kzalloc(sizeof(struct elantech_data), GFP_KERNEL);
	if (!etd)
		return -ENOMEM;

	etd->parity[0] = 1;
	for (i = 1; i < 256; i++)
		etd->parity[i] = etd->parity[i & (i - 1)] ^ 1;

	/*
	 * Do the version query again so we can store the result
	 */
	if (synaptics_send_cmd(psmouse, ETP_FW_VERSION_QUERY, param)) {
		pr_err("failed to query firmware version.\n");
		goto init_fail;
	}

	etd->fw_version = (param[0] << 16) | (param[1] << 8) | param[2];

	if (etd->fw_version < 0x020030) {
		etd->hw_version = 1;
		etd->paritycheck = 1;
	} else if ((etd->fw_version & ~FW_VERSION_MICRO_MASK) <= 0x140000) {
		etd->hw_version = 2;
		/* For now show extra debug information */
		etd->debug = 1;

		if (etd->fw_version >= 0x020800)
			etd->reports_pressure = true;
	} else {
		unsigned char dummy[3];
		error = 0;
		if (ps2_command(&psmouse->ps2dev, NULL, ETP_PS2_CUSTOM_COMMAND) ||
		    ps2_command(&psmouse->ps2dev, NULL, 0x01) ||
		    ps2_command(&psmouse->ps2dev, param, PSMOUSE_CMD_GETINFO))
			error = -1;
		else if (
		    ps2_command(&psmouse->ps2dev, NULL, ETP_PS2_CUSTOM_COMMAND) ||
		    ps2_command(&psmouse->ps2dev, NULL, 0x04) ||
		    ps2_command(&psmouse->ps2dev, dummy, PSMOUSE_CMD_GETINFO))
			error = -1;

		if (!error &&
		    (param[0] & 0x0f) >= 5 && (param[1] & 0x0f) >= 0x06) {
			etd->hw_version = 3;
			etd->debug = 1;
			etd->reports_pressure = true;
		} else {
			pr_debug("did not detect elantech firmware version\n");
			goto init_fail;
		}
	}

	pr_info("assuming hardware version %d, firmware version %d.%d.%d\n",
		etd->hw_version, param[0], param[1], param[2]);

	if (synaptics_send_cmd(psmouse, ETP_CAPABILITIES_QUERY, param)) {
		pr_err("failed to query capabilities.\n");
		goto init_fail;
	}
	pr_info("Synaptics capabilities query result 0x%02x, 0x%02x, 0x%02x.\n",
		param[0], param[1], param[2]);
	etd->capabilities = param[0];

	/*
	 * This firmware suffers from misreporting coordinates when
	 * a touch action starts causing the mouse cursor or scrolled page
	 * to jump. Enable a workaround.
	 */
	if (etd->fw_version == 0x020022 || etd->fw_version == 0x020600) {
		pr_info("firmware version 2.0.34/2.6.0 detected, enabling jumpy cursor workaround\n");
		etd->jumpy_cursor = true;
	}

	if (elantech_set_absolute_mode(psmouse)) {
		pr_err("failed to put touchpad into absolute mode.\n");
		goto init_fail;
	}

	error = elantech_set_input_params(psmouse);
	if (error) {
		pr_err("failed to set input parameters\n");
		goto init_fail;
	}

	error = sysfs_create_group(&psmouse->ps2dev.serio->dev.kobj,
				   &elantech_attr_group);
	if (error) {
		pr_err("failed to create sysfs attributes, error: %d.\n", error);
		goto init_fail;
	}

	psmouse->protocol_handler = elantech_process_byte;
	psmouse->disconnect = elantech_disconnect;
	psmouse->reconnect = elantech_reconnect;
	psmouse->pktsize = etd->hw_version == 1 ? 4 : 6;

	return 0;

 init_fail:
	kfree(etd);
	return -1;
}
