driver  for  MTK  platform.
KK  : for Android 4.x
L : for Android 5.x
Level APK : gsensor auto calibration

L版本說明：

（1）請將文檔中的內容增加到codebase中相應路徑下的文檔中；

（2）alps\device\mediatek\common\sepolicy\untrusted_app.te 此文檔中的內容是為vendor的校準apk增加的，若不使用vendor的校準apk，可以不增加此文檔的內容；

（3）客制化部分mach\mt6735\k35v1_64是以：
platform：mt6735 
project：k35v1_64
為例寫的路徑，實際使用時請換成實際使用的platform和project。