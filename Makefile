#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := esp_blynk

CFLAGS += -DLWIP_NETIF_LOOPBACK=1
CFLAGS += -DLWIP_LOOPBACK_MAX_PBUFS=8

include $(IDF_PATH)/make/project.mk

