##############################################################
#
# dsp_app
#
##############################################################

DSP_APP_VERSION = '1.0'
DSP_APP_SITE = $(BR2_EXTERNAL_PROJECT_BASE_PATH)/../dsp_app
DSP_APP_SITE_METHOD = local
DSP_APP_DEPENDENCIES = alsa-lib libsndfile soundtouch

define DSP_APP_BUILD_CMDS
	$(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) all
endef

define DSP_APP_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 $(@D)/dsp_app $(TARGET_DIR)/usr/bin/
endef

$(eval $(generic-package))
