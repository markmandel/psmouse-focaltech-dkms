#!/bin/bash

source ./dkms.conf
DIR="psmouse-focaltech-dkms"
INSTALL_DIR="/usr/src/$PACKAGE_NAME-$PACKAGE_VERSION"

echo "Installing $PACKAGE_NAME, $PACKAGE_VERSION"

echo "Copying to $INSTALL_DIR"

sudo cp -r ../$DIR $INSTALL_DIR

sudo dkms add -m $PACKAGE_NAME -v $PACKAGE_VERSION && \
sudo dkms build -m $PACKAGE_NAME -v $PACKAGE_VERSION && \
sudo dkms install -m $PACKAGE_NAME -v $PACKAGE_VERSION





