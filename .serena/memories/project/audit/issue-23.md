# 23. «write 16 МБ + read-back + chroot» написан 2-3 раза

(ERRORS.md #15, аудит 2026-11-23.) Цикл записи с ретраями + байт-побайтный read-back + mount/chroot/apt-обвязка в нескольких копиях: flash-emmc.sh/flash-at.sh (flash-boot.sh — удалён, см. 11); build-gst-rockchip.sh/build-v4l2-request.sh/build-rkdeveloptool-arm64.sh.
**Проблема.** Quirk «потеря записи >32 МБ на RK3399Pro» (docs/FLASHING.md) правится в одном файле и пропускается в других.
**Отложено.** Чистый dedup-рефакторинг: цикл записи/read-back в rockchip-usb.sh, chroot-обвязку в общий скрипт; требует полный цикл backup → flash → verify на плате.
**Запись истории:** Отложена, 2026-11-24. (lsusb-guard при этом вынесен в check_usb — см. 28.)

**Сверка всех карточек, 2026-11-24, f86b309.** Циклы записи и read-back остались в flash-at.sh и flash-emmc.sh; независимые chroot-обвязки сохранились в build-gst-rockchip.sh и build-v4l2-request.sh. Условие возврата — тестовый цикл backup, flash и verify после объединения.

**Повторная проверка 2026-11-24. На проверке.** flash-at.sh и flash-emmc.sh используют write_and_verify из rockchip-usb.sh, три arm64-сборщика — build-chroot.sh. На файловой модели eMMC проверены возобновление, неполный сектор, несовпадение в старом префиксе, три попытки при текстовом отказе и при ненулевом коде. На тестовой плате требуется согласованный цикл backup, flash и read-back.

Все три штатные ARM64 chroot-сборки выполнены в отдельных рабочих каталогах: rkdeveloptool, плагины GStreamer Rockchip и libva-v4l2-request. Повтор каждого вызова сообщил up to date. Тест сравнения также принимает расхождение только в MiB 3 полного образа, отклоняет его для записи по LBA и отклоняет короткое чтение.
