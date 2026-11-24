# 19. Клоны U-Boot и TF-A не следят за sources.lock

(ERRORS.md #11, аудит 2026-11-23.) Клонируются один раз и дальше обходятся без сети; бумп UBOOT_TAG/TFA_TAG молча строит старый код.
**Исправление.** 5709764: rev-parse HEAD != пин → fetch --depth 1 + checkout --force (scripts/fetch-sources.sh, scripts/build-tfa.sh) — тот же паттерн, что во всех fetch-скриптах.
**Статус:** Закрыта (локально), 2026-11-24. Проверка: bash -n, логика по исходникам.
