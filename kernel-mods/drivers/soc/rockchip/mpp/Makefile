# SPDX-License-Identifier: (GPL-2.0+ OR MIT)

rk_vcodec-objs := mpp_service.o mpp_common.o mpp_iommu.o
CFLAGS_mpp_service.o += -DMPP_VERSION="\"tinker-edge-r port of Rockchip BSP 6.12 mpp\""

rk_vcodec-$(CONFIG_ROCKCHIP_MPP_RKVDEC) += mpp_rkvdec.o
rk_vcodec-$(CONFIG_ROCKCHIP_MPP_VDPU2)  += mpp_vdpu2.o
rk_vcodec-$(CONFIG_ROCKCHIP_MPP_VEPU2)  += mpp_vepu2.o

obj-$(CONFIG_ROCKCHIP_MPP_SERVICE) += rk_vcodec.o
