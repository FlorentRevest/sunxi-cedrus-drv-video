Sunxi Cedrus VA backend
=======================

This libVA video driver is designed to work with the v4l2 "sunxi-cedrus" kernel driver.

If you want to try it you can use the libva's tests as follow:

	export LIBVA_DRIVER_NAME=sunxi_cedrus
	libva-0.39.2/test/decode/mpeg2vldemo

You can also run more complex applications like VLC media player on a MPEG2 file like one of these:

	http://samplemedia.linaro.org/MPEG2/
