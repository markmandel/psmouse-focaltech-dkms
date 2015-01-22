# psmouse-focaltech DKMS
## Description
Attempt to turn https://github.com/mgottschlag/linux 's Focaltech driver into a
DKMS Driver for Ubuntu 14.10 (may work on older versions like 14.04)

Tested on 14.10

A .deb package can be download at https://github.com/nieluj/psmouse-focaltech-dkms/releases/download/0.1/psmouse-focaltech-dkms_0.1_all.deb

## Installation instructions

```bash
$ sudo apt-get install dkms
$ cd /tmp
$ git clone https://github.com/nieluj/psmouse-focaltech-dkms.git 
$ cd psmouse-focaltech-dkms
$ ./install.sh
```

## Uninstallation
```bash
$ sudo dkms remove psmouse-focaltech/0.1 --all
```
