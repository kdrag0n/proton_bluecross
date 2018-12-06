ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)

# V2.1 SDM845
dtbo-$(CONFIG_BOARD_B1C1) += \
	sdm845-b1-dvt1.1.dtbo \
	sdm845-c1-dvt1.1.dtbo

sdm845-b1-dvt1.1.dtbo-base		:= ../qcom/sdm845-v2.1.dtb
sdm845-c1-dvt1.1.dtbo-base		:= ../qcom/sdm845-v2.1.dtb

endif

always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb
