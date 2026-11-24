# 28. lsusb без проверки наличия инструмента

(ERRORS.md #20, аудит 2026-11-23.) Пять скриптов (emmc-write-test, flash-at, flash-boot, inspect-emmc, verify-emmc) зовут lsusb без `command -v` — неверный диагноз «put the board into maskrom» на машине без usbutils.
**Исправление.** 9db8f37: check_usb() с гвардией lsusb вынесен в общий rockchip-usb.sh и используется всеми (flash-boot.sh при этом удалён — см. 11).
**Запись истории:** Закрыта, 2026-11-24.
