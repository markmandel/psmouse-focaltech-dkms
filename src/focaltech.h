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

#ifndef _FOCALTECH_H
#define _FOCALTECH_H

/*
 * Packet types - the numbers are not consecutive, so we might be missing
 * something here.
 */
#define FOC_TOUCH 0x3 /* bitmap of active fingers */
#define FOC_ABS 0x6 /* absolute position of one finger */
#define FOC_REL 0x9 /* relative position of 1-2 fingers */

#define FOC_MAX_FINGERS 5

#define FOC_MAX_X 2431
#define FOC_MAX_Y 1663

static inline int focaltech_invert_y(int y)
{
	return FOC_MAX_Y - y;
}

/*
 * Current state of a single finger on the touchpad.
 */
struct focaltech_finger_state {
	/* the touchpad has generated a touch event for the finger */
	bool active;
	/*
	 * The touchpad has sent position data for the finger. Touch event
	 * packages reset this flag for new fingers, and there is a time
	 * between the first touch event and the following absolute position
	 * packet for the finger where the touchpad has declared the finger to
	 * be valid, but we do not have any valid position yet.
	 */
	bool valid;
	/* absolute position (from the bottom left corner) of the finger */
	unsigned int x;
	unsigned int y;
};

/*
 * Description of the current state of the touchpad hardware.
 */
struct focaltech_hw_state {
	/*
	 * The touchpad tracks the positions of the fingers for us, the array
	 * indices correspond to the finger indices returned in the report
	 * packages.
	 */
	struct focaltech_finger_state fingers[FOC_MAX_FINGERS];
	/*
	 * True if the clickpad has been pressed.
	 */
	bool pressed;
};

struct focaltech_data {
	unsigned int x_max, y_max;
	struct focaltech_hw_state state;
};

int focaltech_detect(struct psmouse *psmouse, bool set_properties);
int focaltech_init(struct psmouse *psmouse);
bool focaltech_supported(void);

#endif
