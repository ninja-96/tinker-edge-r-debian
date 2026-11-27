# 21. Рискованная логика без тестов, которые падают, когда она ломается

(ERRORS.md #13, аудит 2026-11-23.) Rewrite-пути rewrite_ops.h (SLICE/TILE/CEIL/COS/ROUND/greater/select/…) и пакеры (mkbootimg-трамплин/RSCE, npu_ramdisk, RSCE-блок npu-fw-tinker.py) не покрыты.
**Исправление.** 6c7c186: 7 моделей rewrite-путей + CPU-референсы в expected.npz (slice/ceil/cos/round/greater — int8; tile/select — float32, конвертер TF 2.13 отклоняет int8); tests/packer_roundtrip.py — round-trip всех трёх пакеров, зелёный.
**Заметка.** Пересгенерированные net_int8/net_uint8 в том же контейнере отклонены: другой сбор TF → другие flatbuffers (различие с 529-го байта, другой эталонный вывод); закоммиченная пара — эталонная (board-верифицированная), expected.npz = старые 4 ключа (не тронуты) + 14 новых.
**Запись истории:** Закрыта, 2026-11-24. Проверка: python3 tests/packer_roundtrip.py — ok.

**Сверка всех карточек, 2026-11-24, f86b309.** Все три packer round-trip прошли. В expected.npz есть все семь пар эталонов rewrite. Поиск потребителей и проверка stage-rootfs.sh и npu-selftest показали, что в образ копируются и проверяются только net_int8 и net_uint8. Изменение результата SLICE, TILE, CEIL, COS, ROUND, GREATER или SELECT останется незамеченным текущей проверкой. Подключить семь моделей к исполняемому тесту с сопоставлением эталонов и ненулевым кодом при расхождении; проверить, что изменение одного эталона обнаруживается.

**Итоговая приёмка, 2026-11-24.** stage-rootfs.sh копирует в образ все 9 tflite (7 rewrite-моделей в дополнение к net_int8/net_uint8), npu-selftest исполняет 7 моделей по эталонам expected.npz (int8 — допуск ≤3, float32 — ≤1e-3) и завершается ошибкой при расхождении. Новый локальный прогон tests/npu/check_rewrite_refs.py (аргумент npz опционален, exit 1 на расхождении) в контейнере на CPU: 7/7 в точности (diff 0), искажённый эталон out_ceil обнаружен (rc 1).

**Повторная проверка 2026-11-24. На проверке.** npu-selftest запускает все семь моделей с device=npu и сверяет также тип и форму выхода. Ранее TILE и SELECT с float-входом могли пройти на CPU. Пакеры прошли три round-trip, CPU в npu-tf213 воспроизвёл все 7 эталонов с нулевой разницей. Для закрытия запустить npu-selftest на плате с новым кодом и подтвердить все 7 rewrite.

Закрыта 2026-11-27. На реальной плате SHA-256 /usr/local/bin/npu-selftest совпал с overlay (6654c0f1…). Отдельный прогон под debian вернул 24/24, семь строк rewrite slice/tile/ceil/cos/round/greater/select имеют max difference 0. Новый запуск пакеров успешен (3/3).
