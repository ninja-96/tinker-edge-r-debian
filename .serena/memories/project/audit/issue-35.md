# 35. Chromium в новом rootfs отстаёт от трека Debian security

В предыдущем build-rootfs.sh security и updates прописывались лишь в customize-hook после установки пакетов. Проверка обновлений читала только основной индекс. На 2026-11-27 релизный Chromium 156.0.8078.79, в security уже 156.0.8078.108. Исправлены scripts/build-rootfs.sh и scripts/check-updates.sh. Приёмка — подтвердить источники и версии dpkg в новом образе, сравнить с архивами.

Закрыта 2026-11-27. Проверены список источников из готового ext4 (trixie, trixie-security, trixie-updates), packages.txt и ответ scripts/check-updates.sh: chromium arm64 156.0.8078.108-1~deb13u1. Раздел образа и проверенный ext4 побайтно совпали.
