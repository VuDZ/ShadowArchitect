# Фоновый прогон Satisfactory с SaveCheck

Для проверки размещений и сохранений используется обычный Windows Steam-клиент с SML. PowerShell запускает клиент с внеэкранным рендерингом, а отдельный C++-плагин читает состояние загруженного мира через Unreal Engine. В прогоне обновления 0.1.0 → 0.1.1 этот способ подтвердил загрузку двух планов и создание двух акторов проекции. [Исходник SaveCheck](../tests/compatibility/SaveCheck/Source/SaveCheck/Private/SaveCheck.cpp) · [Результаты контрольных прогонов](save-compatibility.md)

## Подготовка из чистого клона

1. Подготовьте совместимые Steam, SML и Starter Project по инструкции сборки. Установите проверяемую сборку Shadow Architect. Для исходной серии использовались Satisfactory CL 520140 и SML 3.12.1. [Сборка и установка](../README.md)
2. Поместите каталог `tests/compatibility/SaveCheck` в `Mods/SaveCheck` **Starter Project**. Соберите и упакуйте его Alpakit для `FactoryGameSteam / Win64 / Shipping`. Распакуйте Windows-пакет в рабочую папку теста. Рядом с `SaveCheck.uplugin` проверьте `Binaries/Win64`: там нужны DLL модуля SaveCheck и файл `FactoryGameSteam-Win64-Shipping.modules` с записью этой DLL и `BuildId` равным `SML`. Дескриптор из чистого клона содержит исходники плагина; запускайте клиент с путём к **собранной** копии. [Тестовый плагин](../tests/compatibility/SaveCheck/)
3. Подготовьте отдельный тестовый сеанс с известными размещениями, запишите их GUID и число. Сохраните тестовый слот под уникальным именем, например `SAQ_INPUT_01`, в каталоге сохранений используемой учётной записи. Оставьте исходное сохранение отдельно. Скопируйте нужные пары `.sbp` и `.sbpcfg` в тестовый каталог Blueprint. Параметр `-UserDir` отделяет профиль запуска; в проведённых опытах клиент читал и записывал слоты в обычный каталог `FactoryGame/Saved/SaveGames`. Отдельное имя **сеанса** также отделяет его автосохранения на этапе загрузки.
4. Запускайте одну игровую проверку за раз, когда Steam открыт, а игра пользователя закрыта. В Git находятся исходники тестового модуля и этот пример. Архивы релизов, персональные слоты, профили, журналы и прежний машинно-зависимый `run.ps1` лежат вне истории Git. Старые слоты `SAQ_…` из протокола не являются входными данными чистого клона.

## Один запуск на чтение

Пример для PowerShell на Windows. Подставьте пути к распакованному плагину и отдельной библиотеке, имя **уже существующего тестового слота** без `.sav` и имя его тестового сеанса. Используйте простые имена слота и сеанса без пробелов. Для каждого запуска создаётся новый каталог журнала.

```powershell
$ErrorActionPreference = 'Stop'
$probe = (Resolve-Path -LiteralPath 'C:/QA/SaveCheck/SaveCheck.uplugin').Path
$library = (Resolve-Path -LiteralPath 'C:/QA/Blueprints').Path
$slot = 'SAQ_INPUT_01'
$session = 'SAQ01'
$saveRoot = Join-Path $env:LOCALAPPDATA 'FactoryGame/Saved/SaveGames'
$inputs = @(Get-ChildItem -LiteralPath $saveRoot -File -Recurse -Filter "$slot.sav")
if ($inputs.Count -ne 1) { throw "Нужен один однозначный тестовый слот $slot" }
if (Get-Process -Name 'FactoryGame*' -ErrorAction SilentlyContinue) {
    throw 'Игра уже запущена. Отложите тест до закрытия текущей сессии.'
}
$steam = (Get-Process -Name steam | Select-Object -First 1).Path
$run = Join-Path $env:TEMP ('ShadowArchitect-QA-' + [guid]::NewGuid())
$profile = New-Item -ItemType Directory -Path (Join-Path $run 'Profile') -Force
$log = Join-Path $run 'world.log'
$map = '/Game/FactoryGame/Map/GameLevel01/Persistent_Level.Persistent_Level'
$section = '[/Script/EngineSettings.GameMapsSettings]'
$options = "?loadgame=${slot}?sessionName=$session"
$gameArgs = @(
    '-applaunch', '526870', ('"-UserDir={0}"' -f $profile.FullName),
    ('"-ABSLOG={0}"' -f $log), ('"-FGBlueprintDir={0}"' -f $library),
    ('"-PLUGIN={0}"' -f $probe), '-EnablePlugins=SaveCheck', '-SaveCheckRun',
    "-ini:Engine:${section}:GameDefaultMap=$map,${section}:LocalMapOptions=$options",
    '-unattended', '-NoSplash', '-NoSound', '-RenderOffscreen', '-windowed',
    '-ResX=800', '-ResY=450', '-NoCrashDialog'
)
Start-Process -FilePath $steam -ArgumentList $gameArgs -WindowStyle Hidden
Write-Output $log
```

Возврат из `Start-Process` подтверждает только передачу запроса Steam. Дождитесь в новом журнале строк `LogSaveCheck` и завершения клиента. Плагин ждёт начала игры на `Persistent_Level` и появления сеанса сохранения, отключает автосохранение тестового мира, читает данные и запрашивает выход. Для просмотра результата в той же оболочке:

```powershell
Select-String -LiteralPath $log -Pattern 'LogSaveCheck:|LogShadowArchitect:|LogExit: Exiting|Fatal error:'
```

Если нужных строк нет, проверьте фактически открытый слот, путь к DLL, манифест, загрузку модуля и ошибки в этом журнале. Пустой вывод, истечение времени ожидания или один лишь `Ready` оставляют результат проверки открытым.

## Как читать результат

| Запись | Что подтверждает |
|---|---|
| `version=0.1.1 enabled=1` | Клиент обнаружил эту версию Shadow Architect и включил её. При выключении проверяйте `enabled=0`; установленный дескриптор всё ещё может сообщать версию. |
| `blueprints=N`, `blueprint=…` | Число и имена чертежей, которые видит игра в этом запуске. Это контроль входных условий при удалении и переименовании. |
| `plan=0 digest=…` | Отпечаток текстового представления одной записи `Plans`. При одинаковых схемах сравнивайте набор отпечатков с учётом повторов. При смене схемы сопоставьте GUID, позы, состав и новые поля отдельно. Для такого отчёта может потребоваться дополнительный вывод. |
| `subsystems=1 views=2 visible=2` | Найдена одна подсистема, два актора проекций и у обоих снят флаг скрытия. Качество изображения, перекрытие и видимость с конкретной камеры проверяются по кадру. |
| `write=1` | Игра вызвала callback успешной записи. Для проверки восстановления загрузите именно выходной слот в следующем процессе. |

## Три варианта прогона

- **Q01, обновление.** Запишите новый слот старой версией, проверьте версию писателя в метаданных файла, затем прочитайте его новой версией. При испытании 0.1.0 сработал запуск через `FactoryGameSteam.exe` из отдельной установки, где старый мод лежал в штатном `FactoryGame/Mods/ShadowArchitect`. Подключение старой DLL внешним плагином в предварительном опыте дало две подсистемы; такой результат исключили. [Карточка Q01](tasks/q-save-compatibility/01-load-older-save.md)
- **Q02, исходники чертежей.** Сначала снимите контрольные значения. Затем загрузите тот же слот с отдельной изменённой библиотекой в `-FGBlueprintDir`. Проверьте фактически перечисленные имена, записи планов и акторы. [Карточка Q02](tasks/q-save-compatibility/02-rename-or-remove-blueprint.md)
- **Q03, отключение и запись.** До `Start-Process` добавьте к `$gameArgs` два отдельных аргумента: `-DisablePlugins=ShadowArchitect` и `-SaveCheckWrite=SAQ_<новое_имя>`. Выберите свободное имя. Плагин проверяет префикс и существование слота. Дождитесь `write=1` и завершения процесса, укажите выходной слот в `$slot`, уберите оба аргумента и повторите запуск. Для 0.1.1 в контрольном мире результат был 2 плана в исходном слоте и 0 в пересохранённом. [Карточка Q03](tasks/q-save-compatibility/03-resave-with-mod-disabled.md)

Для проверки F7 выберите чертёж в Build Gun, нажмите F7 и сверьте появившуюся проекцию с записью `Placed` в журнале. Для Wireframe или экранной метки приложите кадр клиентской сборки. При проверке списания ресурсов, привязки к поверхности и реальной постройки снимайте соответствующие значения в игре или добавьте измерение в тестовый модуль. В отчёт переносите только относящиеся к опыту строки журнала: полный лог запуска может содержать сведения об учётной записи.

## V03. Проверка DLL из установочного архива

Установите Shadow Architect из проверяемого ZIP в отдельную копию Steam-клиента. SaveCheck соберите из этого репозитория и подключите отдельным плагином через `-PLUGIN`. Возьмите тестовый слот с двумя видимыми планами и доступным фундаментом рядом, например `SAQ_BASE_01` с сеансом `SAQ01`. Поместите один Blueprint вместе с его `.sbpcfg` в отдельный `-FGBlueprintDir`. В контрольном прогоне использовался «2 сборщика кабелей».

В команде фонового запуска замените `-SaveCheckRun` на `-SaveCheckViews=<абсолютный путь к новому каталогу кадров>`. Для отдельной установки запустите её `FactoryGameSteam.exe` при открытом Steam, опустив `-applaunch 526870`. SaveCheck считывает планы через свойства Unreal, подаёт клавиши через обработчик ввода игрока и снимает два кадра Ghost. Завершение сценария подтверждают `installed_module=1`, строки переходов и `pass=1 completed` в `LogSaveCheckViews`. Сверьте указанный `loaded_module` с каталогом установки и сравните SHA-256 этой DLL с DLL из ZIP до и после запуска. [Исходник проверки](../tests/compatibility/SaveCheck/Source/SaveCheck/Private/ViewCheck.cpp)

Для второго прогона добавьте `-SaveCheckCustomKeys` и поместите этот блок в `Saved/Config/Windows/Game.ini` отдельного профиля `-UserDir`.

```ini
[/Script/ShadowArchitect.ShadowPlanSubsystem]
PlaceKey=F7
PlaceChord=(Key=F8,bAlt=True)
SelectKey=F5
MoveChord=(Key=F6,bShift=True)
DeleteChord=(Key=F10,bCtrl=True)
VisibilityChord=(Key=F9,bAlt=True)
ViewChord=(Key=F11,bAlt=True)
```

## B02–B04. Строительство по плану

Соберите текущие версии Shadow Architect и SaveCheck. Используйте отдельную установку клиента, слот V03 `SAQ_BASE_01` с двумя сохранёнными планами рядом с фундаментом и одиночный `QA_A` из [опыта B01](tasks/archive/b-build-from-plan/01-identify-source-blueprint.md). В примере фонового запуска замените `-SaveCheckRun` одним из параметров ниже. SaveCheck отключает автосохранение и завершает клиент после замеров. Ищите `LogSaveCheckBuild` и `LogShadowArchitect` в журнале.

- `-SaveCheckBuild=seed:SAQ_B02_SOURCE_01` нажимает F7, записывает одну новую запись и сохраняет её в свободный тестовый слот. В журнале должны совпасть GUID и отпечаток захвата. Для следующих запусков той же записи загрузите созданный слот.
- `-SaveCheckBuild=source:found:QA_A` и `source:found:QA_A_renamed` проверяют имя открывшегося предпросмотра. Для `source:missing`, `source:replaced` и `source:duplicate` сверяйте причину из `LogShadowArchitect`, отсутствие предпросмотра, прежний GUID и запасы. Сценарий `source:legacy` использует первый план исходного слота без отпечатка.
- Для `-SaveCheckBuild=source:live-replaced` создайте **новую одноразовую копию** библиотеки с парой `QA_A`. Рядом положите другой `.sbp` под именем `QA_B.replacement`. Проверка заменяет только `QA_A.sbp` в этой копии после первого успешного открытия и повторяет Ctrl+Shift+F8 для того же GUID.
- `-SaveCheckBuild=preview` загружает исходный слот, сохраняет два тестовых плана в разных позах, открывает Ctrl+Shift+F8 и отменяет Escape. Журнал сравнивает мировой якорь и детали после обновления предпросмотра, число проекций, реальные здания и запасы до и после отмены.
- `-SaveCheckBuild=confirm` загружает исходный слот и использует штатный ЛКМ с удержанием и быстрым повтором. В сеансе SaveCheck подбирает свободную позу и готовит запас материалов. По журналу сверяйте детали постройки, списание каждой позиции, штатные отказы и взаимодействие E. В тестовом клиенте с дополнительными модами Build Gun можно отключить их параметром `-DisablePlugins=InfiniteZoop,InfiniteNudge,SmartFoundations`.
- `-SaveCheckBuild=confirm-replaced` использует такую же одноразовую библиотеку, как `source:live-replaced`. SaveCheck подменяет файл уже открытого предпросмотра с достаточным запасом материалов и нажимает ЛКМ. Ожидаются отказ из-за изменённого источника, прежнее число объектов и нулевой расход.

Успешный сценарий оканчивается строкой `pass=1`. Пересохранение входного слота для сценариев `source`, `preview` и `confirm` не требуется.

## K02 и K05. Действия и меню плана

Используйте отдельный клиент, слот `SAQ_BASE_01` с двумя планами и тестовую библиотеку с одной парой `QA_A.sbp` и `QA_A.sbpcfg`. Загружайте этот слот заново в каждом прогоне. SaveCheck отключает автосохранение. В журнале ищите `LogSaveCheckMenu` и `LogShadowPlanMenu`.

- `-SaveCheckPlanActions=defaults` проверяет пять действий на исходной раскладке. В журнале видны два GUID выбора, новая поза B, переходы видимости, переход на Wireframe и удалённый GUID.
- `-SaveCheckPlanActions=rebind` записывает пять новых назначений через штатный API игры. После завершения запустите `-SaveCheckPlanActions=custom` с тем же `-UserDir`. Этот прогон проверяет работу новых клавиш и отсутствие действий по прежним.
- `-SaveCheckPlanActions=ui` повторяет проверки после ручного назначения F2, F3, F4, F5 и F9 в штатном меню для выбора, переноса, удаления, видимости и вида соответственно. Запустите с профилем, в котором сохранены эти назначения.
- `-SaveCheckPlanActions=safety` использует профиль из прогона `rebind` и проверяет Ctrl+Shift+F4 с выбранным планом при вводе в чат и в настройках игры.
- `-SaveCheckPlanMenu=<абсолютный путь к новому каталогу кадров>` использует текущие назначения для выбора A и открытия меню над B, пишет `Plan-menu.png` с интерфейсом. Затем проверяет Esc, промах, отмену и подтверждение переноса, открытие по переназначенной клавише и удаление B. При исходной F6 тест назначает F5, при F10 назначает F6. Для результата сопоставьте GUID и позы с журналом.

## E01–E06. Редактирование планов

Используйте отдельный клиент, тестовый слот `SAQ_BASE_01`, отдельную библиотеку Blueprint с одной парой `QA_A.sbp` и `QA_A.sbpcfg` и чистый профиль с исходными клавишами. SaveCheck отключает автосохранение. Для сценариев редактирования он очищает размещения в загруженной тестовой копии и ставит новые через F7. Запись выполняется в новые слоты с префиксом `SAQ_`.

- `-SaveCheckEditing=legacy -SaveCheckWrite=SAQ_E01_01` проверяет J/M/V/K на установленной версии **0.1.1**.
- `-SaveCheckEditing=legacy-map -SaveCheckWrite=SAQ_E01_MAP_01` повторяет E01 в отдельном профиле после переназначения штатной карты с M на F11. Сопоставьте перенос B с результатом исходного профиля.
- `-SaveCheckEditing=edit -SaveCheckMoveWrite=SAQ_E04_01 -SaveCheckWrite=SAQ_E05_01` проверяет выбор с обеих сторон, промахи, метку, перенос, отказ без Build Gun и удаление. Для кадров укажите `-SaveCheckEditingFrames=<абсолютный путь к новому каталогу>`.
- `-SaveCheckEditing=clear -SaveCheckWrite=SAQ_E06_01` проверяет число целей в кадре, отмену, подтверждение после поворота камеры и нулевое число целей при скрытых планах. Используйте параметр кадров для снимка панели.
- `-SaveCheckEditing=reload -SaveCheckEditingExpected=2` загружает выходной слот E04. Для E01, E05 и E06 укажите `-SaveCheckEditingExpected=1`. Сравните GUID и позы из `LogSaveCheckEditing` с журналом записи соответствующего слота и проверьте строку открытия предпросмотра исходного Blueprint.

## K01. Размещение после переназначения

Установите ZIP в `FactoryGame/Mods/GameFeatures/ShadowArchitect` отдельного клиента. Сценарий `-SaveCheckViews=<каталог кадров>` проверяет исходные F7 и Ctrl+Shift+G на `SAQ_BASE_01`. Добавьте `-SaveCheckViewCtrlQ=rebind` в отдельном профиле для переназначения через API игры. SaveCheck проверит Q с убранным Build Gun, прежнюю F7 и Ctrl+Q с предпросмотром. Для прогона после перезапуска используйте `-SaveCheckViewCtrlQ=restart` с тем же профилем. Режим `-SaveCheckViewCtrlQ=reset` сбрасывает назначения и размещает план исходной F7.

## K03. Проверка переноса Game.ini

Подготовьте отдельный профиль с фрагментом [из карточки K03](tasks/archive/k-controls-and-menu/03-import-legacy-keybindings.md). Запустите `-SaveCheckKeybindings=legacy`, затем в том же профиле пройдите `customize`, два прогона `custom`, `unbind-place`, `unbound-place`, `reset` и `reset-check`. Значениями для `-SaveCheckKeybindings` служат названия этих сценариев. Оставьте старый блок на диске. В отдельных профилях проверьте `clean` без блока и `mixed` с неверной клавишей размещения и `SelectKey=F5`. Для проверки действий Alt+F8 и Ctrl+Shift+F5 из этого блока используйте в новом профиле `-SaveCheckViews=<каталог кадров> -SaveCheckLegacyKeys`.

## K04. Проверка подсказки

В тех же журналах сравните строки `Ready` с фактическим назначением. Для проверки нужны исходная F7, новое сочетание с модификаторами и строка с `/shadowplace` после снятия клавиши.

## K06. Строительство из меню

В условиях B04 запустите `-SaveCheckBuild=menu-confirm` для проверки нехватки материалов и успешного штатного подтверждения. `-SaveCheckBuild=menu-preview` проверяет отмену. SaveCheck открывает F6 над целевым планом и выбирает `Open build preview`. Сопоставьте GUID со строками `LogShadowPlanMenu` и `LogSaveCheckBuild`.

## S01 и S02. Нативная привязка и поверхность для Build Gun

В отдельном клиенте загрузите копию тестового слота `SAQ_BASE_01` и передайте `-SaveCheckAlignment=<абсолютный путь к новому каталогу кадров>` вместо `-SaveCheckRun`. Отключите дополнительные моды, которые меняют Build Gun. SaveCheck отключает автосохранение, создаёт настоящие детали и их проекции в тестовом режиме и завершает клиент. Выходного слота нет.

`LogSaveCheckAlignment` пишет позы настоящей детали и призрака, измерения предпросмотра и число построек после ЛКМ. `LogSaveCheckSurface` пишет попадание в экспериментальный меш, принятие попадания и причины отказа. `walk=1` означает проход капсулы игрока сквозь включённый меш. `dismantle=1` подтверждает наведение клавишей F на реальное здание после выключения поверхности. Для сравнения используйте одноимённые кадры в выбранном каталоге. [S01](tasks/archive/s-building-alignment/01-measure-native-snapping.md) · [S02](tasks/archive/s-building-alignment/02-test-hologram-surface.md)

Для S03–S04 запустите тот же слот в отдельном клиенте с новой сборкой мода и передайте `-SaveCheckAlignmentGuide=<абсолютный путь к новому каталогу кадров>`. SaveCheck включает ориентир через F6 и пункт меню, измеряет линии относительно штатного предпросмотра, поднимает копию плана, проверяет скат в двух точках, сравнивает две грани фундамента и проходит штатный Nudge. В конце SaveCheck подтверждает ЛКМ две постройки с реальной опорой для Build Gun и сравнивает их позы с сеткой. `LogSaveCheckAlignmentGuide` содержит результаты сценариев, `pass=1 completed` подтверждает успешный прогон. Кадры включают игровой интерфейс. Выходного слота нет. [S03](tasks/archive/s-building-alignment/03-align-to-roof-height.md) · [S04](tasks/archive/s-building-alignment/04-snap-adjacent-foundation.md)

## S05. Второй Blueprint на перекрытии

В отдельном клиенте загрузите `SAQ_BASE_01`, укажите каталог с копией `3х3 пустой.sbp` и `3х3 пустой.sbpcfg` через `-FGBlueprintDir` и передайте `-SaveCheckRoofStack=<абсолютный путь к новому каталогу кадров>`. SaveCheck создаёт нижний план через F7, ставит второй через меню F6 и сравнивает координаты, части и опоры. Затем открывает **Open build preview** на верхнем плане и подаёт штатный ЛКМ. Прогон отключает стоимость и автосохранение в тестовом мире. Для результата смотрите `LogSaveCheckRoofStack`, в том числе `contacts`, `new_groups` и `matched_positions`. Выходного слота нет. [S05](tasks/archive/s-building-alignment/05-stack-blueprint-on-roof.md)

## H01–H04. Размещение на голограммах

Загрузите отдельный тестовый слот `SAQ_BASE_01` и укажите копии пар `.sbp` и `.sbpcfg` для «3х3 пустой», «3 литейных» и «2 сборщика кабелей». Каждому прогону задайте новый профиль `-UserDir` и каталог кадров. Для увеличенного масштаба панели H03 добавьте `-ini:Engine:[/Script/Engine.UserInterfaceSettings]:ApplicationScale=1.35` отдельным аргументом. [Сцена и критерии](tasks/archive/h-hologram-placement/)

- **H01.** `-SaveCheckPlacementSupport=<кадры> -SaveCheckWrite=SAQ_H01_SUPPORT_01` переключает опору, переназначает клавишу и измеряет позы на обоих краях перекрытия. Загрузите выходной слот с `-SaveCheckPlacementSupportReload` и сопоставьте GUID и позу из `LogSaveCheckPlacementSupport`.
- **H02.** `-SaveCheckPlacementConflict=<кадры>` проверяет свободную позу, касание, наложение и штатный запрет. `LogSaveCheckPlacementConflict` выводит статус, цвет предпросмотра и штатные причины отказа.
- **H03.** `-SaveCheckPlanningControls=<кадры>` проверяет строки панели при переключении режима, переназначении, открытии меню и чата. Сопоставьте кадры с `LogSaveCheckPlanningControls`.
- **H04.** `-SaveCheckRoofStack=<кадры> -SaveCheckPlanMenuPointer` подаёт в Slate наведение, нажатие и отпускание ЛКМ в координатах кнопок. Сверьте кадры трёх состояний, `LogShadowPlanMenu` с GUID и результат `LogSaveCheckRoofStack`. Сценарий включает увеличенный масштаб и выход по Esc.
