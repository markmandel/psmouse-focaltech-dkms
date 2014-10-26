/*
 * Focaltech TouchPad PS/2 mouse driver
 *
 * Copyright (c) 2014 Red Hat Inc.
 * Copyright (c) 2014 Mathias Gottschlag <mgottschlag@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Red Hat authors:
 *
 * Hans de Goede <hdegoede@redhat.com>
 */


#include <linux/device.h>
#include <linux/libps2.h>
#include <linux/input/mt.h>
#include <linux/serio.h>
#include <linux/slab.h>
#include "psmouse.h"
#include "focaltech.h"

static const char * const focaltech_pnp_ids[] = {
	"FLT0101",
	"FLT0102",
	"FLT0103",
	NULL
};

/*
 * Even if the kernel is built without support for Focaltech PS/2 touchpads (or
 * when the real driver fails to recognize the device), we still have to detect
 * them in order to avoid further detection attempts confusing the touchpad.
 * This way it at least works in PS/2 mouse compatibility mode.
 */

int focaltech_detect_fallback(struct psmouse *psmouse, bool set_properties)
{
	if (!psmouse_matches_pnp_id(psmouse, focaltech_pnp_ids))
		return -ENODEV;

	if (set_properties) {
		psmouse->vendor = "FocalTech";
		psmouse->name = "FocalTech Touchpad in mouse emulation mode";
	}

	return 0;
}

int focaltech_init_fallback(struct psmouse *psmouse)
{
	ps2_command(&psmouse->ps2dev, NULL, PSMOUSE_CMD_RESET_DIS);
	psmouse_reset(psmouse);

	return 0;
}

#ifdef CONFIG_MOUSE_PS2_FOCALTECH

static int focaltech_read_register(struct ps2dev *ps2dev, int reg,
		unsigned char *param)
{
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_SETSCALE11))
		return -1;
	param[0] = 0;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES))
		return -1;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES))
		return -1;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES))
		return -1;
	param[0] = reg;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES))
		return -1;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO))
		return -1;
	return 0;
}

int focaltech_detect(struct psmouse *psmouse, bool set_properties)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	unsigned char param[3];

	if (!psmouse_matches_pnp_id(psmouse, focaltech_pnp_ids))
		return -ENODEV;

	/*
	 * We don't know how to identify the device, so we just check the
	 * content of all registers.
	 */
	if (focaltech_read_register(ps2dev, 0, param))
		return -EIO;
	if (param[0] != 0x69 || param[1] != 0x80 || param[2] != 0x80)
		return -ENODEV;
	if (focaltech_read_register(ps2dev, 1, param))
		return -EIO;
	if (param[0] != 0x36 || param[1] != 0x53 || param[2] != 0x03)
		return -ENODEV;
	if (focaltech_read_register(ps2dev, 2, param))
		return -EIO;
	/* 0x13 and 0x0d might be the touchpad size? */
	if (param[0] != 0x00 || param[1] != 0x13 || param[2] != 0x0d)
		return -ENODEV;
	if (focaltech_read_register(ps2dev, 5, param))
		return -EIO;
	if (param[0] != 0x0b || param[1] != 0x03 || param[2] != 0x00)
		return -ENODEV;
	if (focaltech_read_register(ps2dev, 6, param))
		return -EIO;
	if (param[0] != 0x23 || param[1] != 0xbd || param[2] != 0xf8)
		return -ENODEV;

	if (set_properties) {
		psmouse->vendor = "FocalTech";
		psmouse->name = "FocalTech Touchpad";
	}

	return 0;
}

static void focaltech_report_state(struct psmouse *psmouse)
{
	int i;
	struct focaltech_data *priv = psmouse->private;
	struct focaltech_hw_state *state = &priv->state;
	struct input_dev *dev = psmouse->dev;
	int finger_count = 0;

	for (i = 0; i < FOC_MAX_FINGERS; i++) {
		struct focaltech_finger_state *finger = &state->fingers[i];
		int active = finger->active && finger->valid;
		input_mt_slot(dev, i);
		input_mt_report_slot_state(dev, MT_TOOL_FINGER, active);
		if (active) {
			finger_count++;
			input_report_abs(dev, ABS_MT_POSITION_X, finger->x);
			input_report_abs(dev, ABS_MT_POSITION_Y,
					focaltech_invert_y(finger->y));
		}
	}
	input_mt_report_pointer_emulation(dev, false);
	input_mt_report_finger_count(dev, finger_count);

	input_report_key(psmouse->dev, BTN_LEFT, state->pressed);
	input_sync(psmouse->dev);
}

static void process_touch_packet(struct focaltech_hw_state *state,
		unsigned char *packet)
{
	int i;
	unsigned char fingers = packet[1];

	state->pressed = (packet[0] >> 4) & 1;
	/* the second byte contains a bitmap of all fingers touching the pad */
	for (i = 0; i < FOC_MAX_FINGERS; i++) {
		if ((fingers & 0x1) && !state->fingers[i].active) {
			/* we do not have a valid position for the finger yet */
			state->fingers[i].valid = 0;
		}
		state->fingers[i].active = fingers & 0x1;
		fingers >>= 1;
	}
}

static void process_abs_packet(struct focaltech_hw_state *state,
		unsigned char *packet)
{
	unsigned int finger = (packet[1] >> 4) - 1;

	state->pressed = (packet[0] >> 4) & 1;
	if (finger >= FOC_MAX_FINGERS)
		return;
	/*
	 * packet[5] contains some kind of tool size in the most significant
	 * nibble. 0xff is a special value (latching) that signals a large
	 * contact area.
	 */
	if (packet[5] == 0xff)
		return;
	state->fingers[finger].x = ((packet[1] & 0xf) << 8) | packet[2];
	state->fingers[finger].y = (packet[3] << 8) | packet[4];
	state->fingers[finger].valid = 1;
}
static void process_rel_packet(struct focaltech_hw_state *state,
		unsigned char *packet)
{
	int finger1 = ((packet[0] >> 4) & 0x7) - 1;
	int finger2 = ((packet[3] >> 4) & 0x7) - 1;

	state->pressed = packet[0] >> 7;
	if (finger1 < FOC_MAX_FINGERS) {
		state->fingers[finger1].x += (char)packet[1];
		state->fingers[finger1].y += (char)packet[2];
	}
	/*
	 * If there is an odd number of fingers, the last relative packet only
	 * contains one finger. In this case, the second finger index in the
	 * packet is 0 (we subtract 1 in the lines above to create array
	 * indices).
	 */
	if (finger2 != -1 && finger2 < FOC_MAX_FINGERS) {
		state->fingers[finger2].x += (char)packet[4];
		state->fingers[finger2].y += (char)packet[5];
	}
}

static void focaltech_process_packet(struct psmouse *psmouse)
{
	struct focaltech_data *priv = psmouse->private;
	unsigned char *packet = psmouse->packet;

	switch (packet[0] & 0xf) {
	case FOC_TOUCH:
		process_touch_packet(&priv->state, packet);
		break;
	case FOC_ABS:
		process_abs_packet(&priv->state, packet);
		break;
	case FOC_REL:
		process_rel_packet(&priv->state, packet);
		break;
	default:
		psmouse_err(psmouse, "Unknown packet type: %02x", packet[0]);
		break;
	}

	focaltech_report_state(psmouse);
}

static psmouse_ret_t focaltech_process_byte(struct psmouse *psmouse)
{
	if (psmouse->pktcnt >= 6) { /* Full packet received */
		focaltech_process_packet(psmouse);
		return PSMOUSE_FULL_PACKET;
	}
	/*
	 * we might want to do some validation of the data here, but we do not
	 * know the protocoll well enough
	 */
	return PSMOUSE_GOOD_DATA;
}

static void focaltech_reset(struct psmouse *psmouse)
{
	psmouse_info(psmouse, "focaltech_reset");
	ps2_command(&psmouse->ps2dev, NULL, PSMOUSE_CMD_RESET_DIS);
	psmouse_reset(psmouse);
}

static int focaltech_switch_protocol(struct psmouse *psmouse)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	unsigned char param[3];

	param[0] = 0;
	if (ps2_command(ps2dev, param, 0x10f8))
		return -EIO;
	if (ps2_command(ps2dev, param, 0x10f8))
		return -EIO;
	if (ps2_command(ps2dev, param, 0x10f8))
		return -EIO;
	param[0] = 1;
	if (ps2_command(ps2dev, param, 0x10f8))
		return -EIO;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_SETSCALE11))
		return -EIO;

	if (ps2_command(ps2dev, param, PSMOUSE_CMD_ENABLE))
		return -EIO;

	return 0;
}

static void focaltech_disconnect(struct psmouse *psmouse)
{
	focaltech_reset(psmouse);
	kfree(psmouse->private);
	psmouse->private = NULL;
}

static int focaltech_reconnect(struct psmouse *psmouse)
{
	focaltech_reset(psmouse);
	if (focaltech_switch_protocol(psmouse)) {
		psmouse_err(psmouse,
			    "Unable to initialize the device.");
		return -1;
	}
	return 0;
}

static void set_input_params(struct psmouse *psmouse)
{
	struct input_dev *dev = psmouse->dev;

	__set_bit(EV_KEY, dev->evbit);
	__set_bit(EV_ABS, dev->evbit);
	input_set_abs_params(dev, ABS_X, 0, FOC_MAX_X, 0, 0);
	input_set_abs_params(dev, ABS_Y, 0, FOC_MAX_Y, 0, 0);
	input_mt_init_slots(dev, 5, INPUT_MT_POINTER);
	input_set_abs_params(dev, ABS_MT_POSITION_X, 0, FOC_MAX_X, 0, 0);
	input_set_abs_params(dev, ABS_MT_POSITION_Y, 0, FOC_MAX_Y, 0, 0);
	__set_bit(BTN_TOUCH, dev->keybit);
	__set_bit(BTN_TOOL_FINGER, dev->keybit);
	__set_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
	__set_bit(BTN_TOOL_TRIPLETAP, dev->keybit);
	__set_bit(BTN_TOOL_QUADTAP, dev->keybit);
	__set_bit(BTN_TOOL_QUINTTAP, dev->keybit);
	__clear_bit(EV_REL, dev->evbit);
	__clear_bit(REL_X, dev->relbit);
	__clear_bit(REL_Y, dev->relbit);
	__set_bit(BTN_LEFT, dev->keybit);
	__clear_bit(BTN_RIGHT, dev->keybit);
	__clear_bit(BTN_MIDDLE, dev->keybit);
	__set_bit(INPUT_PROP_BUTTONPAD, dev->propbit);
}

int focaltech_init(struct psmouse *psmouse)
{
	struct focaltech_data *priv;

	focaltech_reset(psmouse);
	if (focaltech_switch_protocol(psmouse)) {
		focaltech_reset(psmouse);
		psmouse_info(psmouse,
			     "Unable to initialize the device.");
		return -ENOSYS;
	}

	psmouse->private = priv = kzalloc(sizeof(struct focaltech_data), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	set_input_params(psmouse);

	psmouse->protocol_handler = focaltech_process_byte;
	psmouse->pktsize = 6;
	psmouse->disconnect = focaltech_disconnect;
	psmouse->reconnect = focaltech_reconnect;
	psmouse->cleanup = focaltech_reset;
	/* resync is not supported yet */
	psmouse->resync_time = 0;

	return 0;
}

#else /* CONFIG_MOUSE_PS2_FOCALTECH */

int focaltech_detect(struct psmouse *psmouse, bool set_properties)
{
	return -ENOSYS;
}

int focaltech_init(struct psmouse *psmouse)
{
	return -ENOSYS;
}

#endif /* CONFIG_MOUSE_PS2_FOCALTECH */
