# 21. Рискованная логика без тестов, которые падают, когда она ломается

(ERRORS.md #13, аудит 2026-11-23.) Rewrite-пути rewrite_ops.h (SLICE/TILE/CEIL/COS/ROUND/greater/select/…) и пакеры (mkbootimg-трамплин/RSCE, npu_ramdisk, RSCE-блок npu-fw-tinker.py) не покрыты.
**Исправление.** 6c7c186: 7 моделей rewrite-путей + CPU-референсы в expected.npz (slice/ceil/cos/round/greater — int8; tile/select — float32, конвертер TF 2.13 отклоняет int8); tests/packer_roundtrip.py — round-trip всех трёх пакеров, зелёный.
**Заметка.** Пересгенерированные net_int8/net_uint8 в том же контейнере отклонены: другой сбор TF → другие flatbuffers (различие с 529-го байта, другой эталонный вывод); закоммиченная пара — эталонная (board-верифицированная), expected.npz = старые 4 ключа (не тронуты) + 14 новых.
**Запись истории:** Закрыта, 2026-11-24. Проверка: python3 tests/packer_roundtrip.py — ok.
