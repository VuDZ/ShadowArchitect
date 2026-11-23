# Shadow Architect

Планирование фабрики в одиночной Satisfactory. Игрок выставляет пользовательский Blueprint штатным Build Gun и отдельной клавишей фиксирует его проекцию. Исходники рассчитаны на Windows Steam, Satisfactory **CL 520140**, SML **3.12.1** и UE **5.6.1-CSS**. [Совместимость в релизе SML](https://github.com/satisfactorymodding/SatisfactoryModLoader/releases/tag/v3.12.1)

**Локальная сборка 0.1.2 для Steam Windows.** Архив `dist/ShadowArchitect-0.1.2-Steam-Windows.zip` включает Ghost, Wireframe, Footprint и переназначение клавиш. При закрытой игре распакуйте архив в `<игра>/FactoryGame/Mods/ShadowArchitect/` рядом с SML 3.12.1. В этом каталоге должен находиться `ShadowArchitect.uplugin` с `VersionName` равным `0.1.2`. Архивы хранятся локально. Для чистого клона используйте инструкцию сборки ниже. [Проверка V03](docs/tasks/V03.md)

Офлайн прочитаны 25 игровых Blueprint и воспроизведён чертёж из 16 фундаментов. [Исходники](src/ShadowArchitect/) · [Результаты исследования](docs/feasibility.md) · [Архитектура](docs/architecture.md)

[План работ с задачами и критериями приёмки](docs/work-plan.md) · [Исходные пожелания](docs/roadmap.md)

**Перед отключением мода сохраните отдельный слот.** Если записать мир с отключённым Shadow Architect, после включения мода в этом слоте уже не будет размещённых проекций. [Проверка сохранений и обновления](docs/save-compatibility.md)

## Сборка и установка для локального теста

1. Подготовьте Starter Project тега `v3.12.1` с соответствующими бинарными зависимостями, редактором 5.6.1-CSS и интеграцией Wwise 2025.1.6.9117. Руководство указывает VS 2022 и MSVC v143 14.38. [Исходный проект](https://github.com/satisfactorymodding/SatisfactoryModLoader/tree/v3.12.1) · [Зависимости](https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/dependencies.html) · [Подготовка проекта](https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/project_setup.html)
2. При закрытом редакторе поместите `ShadowArchitect.uplugin`, `Config`, `Content` и `Source` из `src/ShadowArchitect` в `<Starter>/Mods/ShadowArchitect/`. На машине разработки вместо копии можно использовать junction на каталог плагина. В `Content` лежат материалы Wireframe, которые Alpakit приготовит для клиента.
3. Обновите project files для `FactoryGame.uproject`. Соберите проект `FactoryGame` в конфигурации **Development Editor | Win64**. Откройте проект в CSS-редакторе. [Конфигурации сборки](https://docs.ficsit.app/satisfactory-modding/latest/Development/Cpp/setup.html)
4. При закрытой игре выберите в **Alpakit Dev** плагин **Shadow Architect**, Windows, каталог Steam-игры и **Copy to Game Path**. Запустите упаковку. Для ручной диагностики клиентская цель — `FactoryGameSteam`, конфигурация **Shipping | Win64**. Установленная версия SML должна совпадать с целевой. Разрабатываемый плагин ставится в `<игра>/FactoryGame/Mods/ShadowArchitect/`.
5. Для передаваемого пакета запустите **Alpakit Release**. В `Config/Alpakit.ini` выбрана цель Windows. Архивы располагаются в `<Starter>/Saved/ArchivedPlugins/ShadowArchitect/`. В платформенном ZIP файл `ShadowArchitect.uplugin` лежит рядом с `Binaries/Win64/`. Содержимое такого ZIP распаковывают в каталог мода из шага 4. Если получен объединённый `ShadowArchitect.zip`, для ручной установки возьмите содержимое его папки `Windows/`. [Код упаковки Alpakit](https://github.com/satisfactorymodding/SatisfactoryModLoader/blob/v3.12.1/Mods/Alpakit/Source/Alpakit.Automation/PackagePlugin.cs)
6. Откройте новый тестовый мир или отдельную копию сохранения. После загрузки ищите сообщение `Ready` в чате и `LogShadowArchitect` в `%LOCALAPPDATA%/FactoryGame/Saved/Logs/FactoryGame.log`. При ошибке загрузки проверьте в этом же журнале сообщения SML о версии и зависимостях.

## Управление

Выберите Blueprint обычным способом, выставьте положение и поворот. Для точной установки можно сначала зафиксировать штатный preview в Nudge Mode. Нажмите **F7**, затем закройте Build Gun или перейдите к следующему модулю. ЛКМ сохраняет своё обычное строительное действие. Для проверки связи с модом уберите Build Gun, откройте чат клавишей Enter и отправьте `/shadowplace`. Ожидаемый ответ — `Select a blueprint with the Build Gun first.`

| Сочетание | Действие |
|---|---|
| F7 или команда чата `/shadowplace` | Place Shadow. Зафиксировать текущий Blueprint. Новый экземпляр сразу выбран. |
| Ctrl+Shift+J | Выбрать следующий план по кругу. В чате появятся название, GUID и координаты в см. |
| Ctrl+Shift+M | Перенести выбранный план в положение и поворот текущего preview. Можно использовать обычный фундамент как ориентир, даже если исходный Blueprint удалён. |
| Ctrl+Shift+K | Удалить выбранный план. Для следующего удаления нужно снова выбрать план. |
| Ctrl+Shift+V | Скрыть или показать все планы в мире. Видимость сохраняется вместе с миром. |
| Ctrl+Shift+G | Переключить вид всех планов по кругу Ghost → Wireframe → Footprint → Ghost. |

При загрузке мира используется Ghost.

При закрытой игре откройте `%LOCALAPPDATA%/FactoryGame/Saved/Config/Windows/Game.ini`. В этом блоке можно указать полные сочетания для всех шести действий. Пример воспроизводит стандартные привязки. Пропущенные модификаторы в строке `Chord` считаются выключенными.

```ini
[/Script/ShadowArchitect.ShadowPlanSubsystem]
PlaceChord=(Key=F7)
SelectChord=(Key=J,bCtrl=True,bShift=True)
MoveChord=(Key=M,bCtrl=True,bShift=True)
DeleteChord=(Key=K,bCtrl=True,bShift=True)
VisibilityChord=(Key=V,bCtrl=True,bShift=True)
ViewChord=(Key=G,bCtrl=True,bShift=True)
```

Существующие `PlaceKey`, `SelectKey`, `MoveKey`, `DeleteKey`, `VisibilityKey` и `ViewKey` работают со стандартными модификаторами. Если заданы и `Key`, и `Chord` для одного действия, используется сочетание из `Chord`. Для Alt и клавиши Windows используйте `bAlt=True` и `bCmd=True`.

## Что можно проверить сейчас

Из корня проекта выполните команды ниже с Python 3.10+. Читалка открывает входной файл только для чтения. Второй вызов выводит классы и число акторов в копии вашего чертежа. Для чертежа только из бетонных фундаментов 8×8×1 м параметр `--obj новое-имя.obj` создаёт отдельную 3D-модель в сантиметрах. [Модель, полученная в эксперименте](docs/foundation-spike.obj)

```powershell
python -B -m unittest discover -s tests -v
python -B scripts/probe_blueprint.py 'D:/TestBlueprints/sample.sbp'
```

В игре сначала проверьте захват пяти зданий с закрытием Build Gun, затем полную перезагрузку процесса. Для повторения теста с удалённым источником используйте отдельный тестовый сеанс и одноразовый Blueprint. [Тесты A–E и критерии приёмки](docs/test-plan.md)
