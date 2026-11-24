# Игровая проверка H01–H04

24 ноября 2026 года. Satisfactory Steam 1.2.5.1, CL `520140`, SML `3.12.1`. Прогоны H01, H02 и H04 выполнены на сборках из `7642562` с изменениями H01–H04 до выпуска 0.3.0. Итоговое оформление H03 проверено на 0.3.0. Установленная DLL 0.3.0 имеет SHA-256 `77EFCD5AA44E75585670F8352D317156F0B322886F5E135771EA6868F4881512`.

Входной слот `SAQ_BASE_01` и библиотека использованы в отдельном клиенте. Контрольная сумма входного слота совпала до и после тестов. [Параметры запуска](../../../background-testing.md#h01h04-размещение-на-голограммах)

## H01. Опора под прицелом

Исходное `Ctrl+Shift+T` включило опору на планы, переназначенная F11 вернула «Только мир». Верх реального пола — 10 143,02 см. Низ предпросмотра на обоих краях перекрытия — 11 243,02 см, на высоте верхней грани. Пройдены Ghost, Wireframe, скрытие, промах, приоритет реальной опоры и отмена.

```text
input=F7 plans=3->4 real=11721->11721 inventory_same=1
upper=102A2D9C4A9FD1DA9D6F76A64B38EE47
shown=-229024.918312,-116578.977387,11292.766762|0.000000,10.000000,0.000000
pass=1 write=1
reload id=102A2D9C4A9FD1DA9D6F76A64B38EE47 count=4
pass=1 stage=0 reloaded last saved plan
```

Выходной слот `SAQ_H01_SUPPORT_02` восстановил ту же позу и GUID. Общая геометрия металлической крыши прошла S03–S04. Отдельный прогон F7 на металлической крыше остаётся открыт.

## H02. Подсветка пересечений

Свободная поза и касание стыка при 11 243,02 см оставались зелёными. При наложении на 60 см весь активный Blueprint стал красным. Контуры выделили участвующие детали предпросмотра красным и детали нижнего плана жёлтым. F7 добавила один план без изменения инвентаря и реальных построек. При наведении на реальную литейную с режимом «Только мир» сохранился штатный запрет. [Кадр наложения](../../../evidence/H02-overlap.png)

```text
case=touch label="Свободно" can_construct=1 bottom_cm=11243.02 roof_cm=11243.02
case=overlap label="Пересечение планов · 3х3 пустой" bottom_cm=11183.02 roof_cm=11243.02
input=F7 plans=3->4 real_buildings=11719->11719 inventory_same=1
case=native-overlap native_reason=FGCDEncroachingClearance
case=native-overlap label="" tint=0 can_construct=0 native_material=2
pass=1 stage=8 free, touch, overlap, F7 and native prohibition
```

Запись слота не выполнялась. Промежуток между колоннами и поворот проверены тестом геометрии Unreal. Отдельные игровые кадры для выступающей колонны и статуса «Возможное пересечение» остаются открытыми.

## H03. Боковая панель

Проверены масштабы 100% и 135%, переключение опоры, исчезновение цели при промахе, скрытие панели в меню и чате. Через API штатных назначений F7 заменена на Ctrl+Alt+F10, привязка F6 снята и возвращена. Сочетание добавило один план, а возвращённая F6 открыла меню. В итоговой панели 0.3.0 видны рамка, описания слева и сочетания справа. Запись слота не выполнялась. [Кадр панели](../../../evidence/H03-planning-controls.png)

```text
case=rebind panel=Опора · Мир и планы | Разместить план · Left Ctrl + Left Alt + F10 | Сменить опору · Right Ctrl + Right Shift + T | Меню плана · Не назначено
input=Ctrl+Alt+F10 plans=4 new_guid=F880634542CDE52EB22887ACFC1442B8
pass=1 stage=13 default, mode change, F6, aim miss, live rebind, chat and no planning context
```

## H04. Меню плана

При 100% и 135% игровой HUD отрисовывался под меню. Наведение, нажатие и отпускание ЛКМ подавались в Slate по координатам виджетов. Щелчки включили и выключили ориентир у одного GUID. Команда крыши добавила один план, Esc вернул ввод Build Gun. Запись слота не выполнялась. [Кадр нажатия](../../../evidence/H04-menu-pressed.png)

```text
Chose Show alignment guide for plan [9F1DB67640992ED42FC8EB9B826D42CC].
Chose Hide alignment guide for plan [9F1DB67640992ED42FC8EB9B826D42CC].
input=LMB action=Place Blueprint on roof target=9F1DB67640992ED42FC8EB9B826D42CC last=B1DBC16A48A078AA0985E5B72464175F real_buildings=11719->11719 inventory_same=1 restored=1
input=Esc plans=4 real_buildings=11719->11719 inventory_same=1 restored=1
pass=1 stage=2 completed plan menu pointer check
```
