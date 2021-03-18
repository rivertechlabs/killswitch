#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#
#V := 1
PROJECT_NAME := killswitch

EXTRA_COMPONENT_DIRS := $(CURDIR)/../esp-idf-lib/components

include $(IDF_PATH)/make/project.mk
