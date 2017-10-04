# Android GPS USB/Serial driver for TinkerBoard
GPS driver for TinkerBoard.

To compile the library, you need the Marshmallow source code.
Once you have, the path to the include folders need to be adjusted in Android.mk.
The library 'libcutils.so' need to be extracted from the TinkerBoard and copied to the jni folder:

```
adb pull /system/lib/libcutils.so
```

Then execute from the jni folder the command: 

```
"/path/to/your/SDK/ndk-bundle/ndk-build -B"
```

You will get the library:

```
[armeabi] Compile thumb  : gps.default <= gps.c
[armeabi] SharedLibrary  : gps.default.so
[armeabi] Install        : gps.default.so => libs/armeabi/gps.default.so
```

Install the library to the /system/lib/hw folder on the TinkerBoard:

```
adb push ../libs/armeabi/gps.default.so /system/lib/hw/
```

The default serial device is the '/dev/ttyACM0' and the serial speed 9600 baud.
To change this, it must be created a file gps.conf in the folder /system/etc/ with this content:

```
GPSPORT /dev/ttyACM0
GPSSPEED 115200
```

You can do this with adb easily:

```
adb shell echo "'GPSPORT /dev/ttyACM0\nGPSSPEED 115200' > /system/etc/gps.conf"
```

The serial device needs the permission 666. To do this on boot, there are alternatives. One is to extract the boot partition and change the file uevent.rk3288.rc and repack and flash back back to the Tinkerboard.
The other solution is to install SuperUser, after there will be a file named 'install-recovery.sh', where you can do this.

