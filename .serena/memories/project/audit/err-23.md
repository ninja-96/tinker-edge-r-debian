# 23. «write 16 МБ + read-back + chroot» написан 2-3 раза

(ERRORS.md #15, аудит 2026-11-23.) Цикл записи с ретраями + байт-побайтный read-back + mount/chroot/apt-обвязка в нескольких копиях: flash-emmc.sh/flash-at.sh (flash-boot.sh — удалён, см. 11); build-gst-rockchip.sh/build-v4l2-request.sh/build-rkdeveloptool-arm64.sh.
**Проблема.** Quirk «потеря записи >32 МБ на RK3399Pro» (docs/FLASHING.md) правится в одном файле и пропускается в других.
**Отложено.** Чистый dedup-рефакторинг: цикл записи/read-back в rockchip-usb.sh, chroot-обвязку в общий скрипт; требует полный цикл backup → flash → verify на плате.
**Статус:** Отложена, 2026-11-24. (lsusb-guard при этом вынесен в check_usb — см. 28.)
