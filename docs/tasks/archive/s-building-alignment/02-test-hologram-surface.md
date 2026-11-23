# S02. Испытать поверхность для луча Build Gun

**Зависимости** — [S01](01-measure-native-snapping.md).

## Цель

На тестовом прототипе проверить доступность поверхности только для запроса Build Gun.

## С чего начать

В `AShadowProjection::Build` отключены коллизия актора и коллизия мешей. Попадание луча, установка позы и разрешение постройки — разные этапы. В SDK исследуйте `IsValidHitResult`, `TrySnapToActor` и `SetHologramLocationAndRotation`. [Рендер проекции](../../../../src/ShadowArchitect/Source/ShadowArchitect/Private/ShadowProjection.cpp) · [Управление планами](../../../../src/ShadowArchitect/Source/ShadowArchitect/Private/ShadowPlan.cpp) · [Исходные пожелания](../../../roadmap.md)

## Работа

1. Сделайте эксперимент на выбранной детали, сохраняя возможность пройти игроком сквозь проекцию. Проверьте, может ли запрос размещения получить её поверхность и нормаль.
2. Для крыши и фундамента запишите по отдельности полученный hit, принятие hit голограммой, конечную позу после обновления и результат штатного подтверждения. Сравните с эталоном S01.

## Независимая проверка

- [x] На обеих деталях проверены попадание и промах. На крыше игра приняла попадание и выставила позу, затем запретила стройку из-за опорной поверхности. Попадания в грани фундамента приняты.
- [x] Капсула игрока прошла сквозь проекцию с включённым прототипом. После выключения клавиша F выбрала настоящее здание за призраком.

## Что передать проверяющему

Сдайте таблицу по двум поверхностям и отдельный выбор для S03 и S04: штатное позиционирование, визуальная направляющая или воспроизводимое ограничение.

## Прототип SaveCheck

`ECC_GameTraceChannel5` в конфигурации SDK соответствует лучу Build Gun. В тестовом режиме SaveCheck включает для меша призрака `QueryOnly` и блокирует только этот канал. Канал доступен и другим состояниям инструмента, поэтому перед проверкой сноса SaveCheck возвращает меш в `NoCollision`. Рабочий мод сохраняет исходный режим коллизии проекции. [Конфигурация канала SDK](https://github.com/satisfactorymodding/SatisfactoryModLoader/blob/v3.12.1/Config/DefaultEngine.ini) · [Прототип SaveCheck](../../../../tests/compatibility/SaveCheck/Source/SaveCheck/Private/AlignmentSurface.cpp)

`TraceForBuilding` даёт попадание. `IsValidHitResult` показывает, принимает ли его голограмма. Штатный `UpdateHologramPlacement` обрабатывает попадание, включая попытку `TrySnapToActor` и вычисление позы через `SetHologramLocationAndRotation`, если специальная привязка не сработала. Фундамент переопределяет оба метода, голограмма станка переопределяет вычисление позы. Отдельно `CanConstruct` и список причин показывают готовность к строительству. Для подтверждения SaveCheck подаёт штатный ЛКМ и отслеживает появление здания. [API голограммы](https://github.com/satisfactorymodding/SatisfactoryModLoader/blob/v3.12.1/Source/FactoryGame/Public/Hologram/FGHologram.h) · [Голограмма фундамента](https://github.com/satisfactorymodding/SatisfactoryModLoader/blob/v3.12.1/Source/FactoryGame/Public/Hologram/FGFoundationHologram.h) · [Голограмма станка](https://github.com/satisfactorymodding/SatisfactoryModLoader/blob/v3.12.1/Source/FactoryGame/Public/Hologram/FGFactoryHologram.h)

## Игровые измерения

23 ноября 2026 года, прогон `S01-final`. Базовая ревизия `3e38d1c` с тестовым сценарием S01–S02 в рабочем дереве. Satisfactory Steam Shipping `rel-main-1.2.6` CL `520140`, SML `3.12.1`, Shadow Architect `0.1.2`. Входной тестовый слот `SAQ_BASE_01`, выходного слота нет. SHA-256 входного слота совпал до и после прогона. Крыша и фундамент повёрнуты на 30° вокруг мировой Z. Для грани +Y использовали отдельный экземпляр фундамента на 1700 см выше первого, чтобы убрать преграды на пути луча. [Запуск и имена журналов](../../../background-testing.md#s01-и-s02-нативная-привязка-и-поверхность-для-build-gun)

| Поверхность и прицел | Попадание в призрак и принятие | Конечная поза относительно S01 | Штатный ЛКМ |
|---|---|---|---|
| Плоская крыша `Build_Roof_A_01`, сверху | Да. Z поверхности `6384,016` см, нормаль `(0, 0, 1)` | Z станка на 1 см выше, yaw `0°` вместо `30°` | `FGCDInvalidFloor`, новых зданий нет |
| Плоская крыша, в сторону неба | Промах. Предпросмотр скрыт, `FGCDInvalidAimLocation` | Позу по поверхности не вычислил | — |
| Фундамент `Build_Foundation_8x1_01`, грань +X | Да. Нормаль `(0.866, 0.500, 0)` | Шаг 401 см вместо 800 см, высота +0,34 см, yaw `0°` вместо `30°` | Построен один фундамент. Габариты перекрылись на 545,41 см вдоль +X |
| Второй фундамент того же типа, грань +Y | Да. Нормаль `(-0.500, 0.866, 0)` | Шаг 401 см вместо 800 см, высота +0,34 см, yaw `0°` вместо `30°` | Построен один фундамент. Габариты перекрылись на 545,41 см вдоль +Y |
| Фундамент, в сторону неба | Промах. Предпросмотр скрыт, `FGCDInvalidAimLocation` | Позу по поверхности не вычислил | — |

Поля журналов `LogSaveCheckSurface` и `LogSaveCheckAlignment` фиксируют попадание, позу и результат ЛКМ.

```text
case=probe-roof  projection_hit=1 accepted=1 can_construct=0
case=probe-roof  disqualifier=FGCDInvalidFloor
case=probe-roof  input=LMB new_buildings=0
case=probe-east  projection_hit=1 accepted=1 can_construct=1
case=probe-east  relative_yaw=-30.00 step_cm=401.00 edge_gap_cm=-545.41
case=probe-east  input=LMB new_buildings=1
case=probe-north projection_hit=1 accepted=1 can_construct=1
case=probe-north relative_yaw=-30.00 step_cm=401.00 edge_gap_cm=-545.41
case=probe-north input=LMB new_buildings=1
case=probe-roof-miss       hit=0 projection_hit=0 accepted=0 visible=0
case=probe-foundation-miss hit=0 projection_hit=0 accepted=0 visible=0
```

Капсула игрока пересекла проекцию фундамента на горизонтальном отрезке 1200 см. После отключения поверхности нажатие F выбрало настоящий фундамент за проекцией. Снос не подтверждался.

```text
walk=1 start=X=-219689.535 Y=-116118.979 Z=6343.016 end=X=-218650.305 Y=-115518.979 Z=6343.016 reached=X=-218650.305 Y=-115518.979 Z=6343.016 blocking=None
dismantle=1 selected=Build_Foundation_8x1_01_C_2147465910 expected=Build_Foundation_8x1_01_C_2147465910
pass=1 completed measured=14 total=14
```

Обе грани и плоская крыша прошли `IsValidHitResult`. Крыша остановилась на проверке опорной поверхности после расчёта позы. Фундамент дошёл до стройки и потерял поворот и шаг родной привязки. Это даёт два самостоятельных основания выбрать визуальный режим **Alignment guide**. Для S03 он указывает высоту грани. Для S04 он показывает линию стыка и положение соседнего фундамента. Разрешение штатной постройки выдаёт игра.

[Индекс задач и условия игрового прогона](../../../work-plan.md)
