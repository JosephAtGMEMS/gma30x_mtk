 driver  for  MTK  platform.
KK  : for Android 4.x
L : for Android 5.x
Level APK : gsensor auto calibration
factory mode:自动校准g-sensor请务必打开,手动校准系统已经添加了。

L版本说明：
 （1）请将文档中的内容增加到codebase中相应路径下的文档中。
 （2）alps/device/mediatek/common/sepolicy/untrusted_app.te  此文档中的内容是为vendor
的校准APK增加的，若不使用vendor的校准APK，可以不增加此文档的内容。
  （3）客制化部分mach/mt6735/k35v1_64是以：
    platform:mt6735
    project:k35v1_64
    为例写的路径，实际使用时请换成实际使用的platform和project.