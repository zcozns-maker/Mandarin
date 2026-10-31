// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <QFrame>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/vk.h"
#include "ui_configure.h"
#include "yuzu/configuration/configure_applets.h"
#include "yuzu/configuration/configure_audio.h"
#include "yuzu/configuration/configure_cpu.h"
#include "yuzu/configuration/configure_debug_tab.h"
#include "yuzu/configuration/configure_dialog.h"
#include "yuzu/configuration/configure_filesystem.h"
#include "yuzu/configuration/configure_general.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/configure_graphics_extensions.h"
#include "yuzu/configuration/configure_hotkeys.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_input_player.h"
#include "yuzu/configuration/configure_network.h"
#include "yuzu/configuration/configure_profile_manager.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/configuration/configure_ui.h"
#include "yuzu/configuration/configure_web.h"
#include "yuzu/hotkeys.h"

namespace {

// Eden-NS2 (ZiK local): the settings tabs - Adv. Graphics especially, with all the
// "Switch 2 / Eden-NS2: ..." entries - can be taller than the screen. A tab's minimum height
// used to become the dialog's minimum height, pushing OK/Cancel/Apply below the screen edge.
// Each tab now lives in a scroll area, so the dialog can be any size and the buttons stay visible.
constexpr char kNs2ScrollWrapper[] = "ns2_scroll_wrapper";

QScrollArea* WrapTabInScrollArea(QWidget* page, QWidget* parent) {
    auto* const scroll = new QScrollArea(parent);
    scroll->setProperty(kNs2ScrollWrapper, true);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setAccessibleName(page->accessibleName());
    scroll->setWidget(page);
    scroll->viewport()->setAutoFillBackground(false);
    page->setAutoFillBackground(false);
    return scroll;
}

// Start at the preferred size but never larger than ~90% of the available screen area,
// centred on the screen of the parent window.
void FitDialogToScreen(QWidget* dialog, const QSize& preferred) {
    QScreen* screen = nullptr;
    if (dialog->parentWidget() != nullptr) {
        screen = dialog->parentWidget()->screen();
    }
    if (screen == nullptr) {
        screen = dialog->screen();
    }
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        dialog->resize(preferred);
        return;
    }
    const QRect available = screen->availableGeometry();
    const QSize size{std::min(preferred.width(), available.width() * 9 / 10),
                     std::min(preferred.height(), available.height() * 9 / 10)};
    dialog->resize(size);
    QRect frame{QPoint{0, 0}, size};
    frame.moveCenter(available.center());
    dialog->move(frame.topLeft());
}

// Returns the scroll area wrapping |tab| (see WrapTabInScrollArea), or |tab| itself.
QWidget* PageFor(QWidget* tab) {
    QWidget* const viewport = tab->parentWidget();
    QWidget* const outer = viewport != nullptr ? viewport->parentWidget() : nullptr;
    auto* const scroll = qobject_cast<QScrollArea*>(outer);
    if (scroll != nullptr && scroll->property(kNs2ScrollWrapper).toBool() &&
        scroll->widget() == tab) {
        return scroll;
    }
    return tab;
}

} // namespace

ConfigureDialog::ConfigureDialog(QWidget* parent, HotkeyRegistry& registry_,
                                 InputCommon::InputSubsystem* input_subsystem,
                                 std::vector<VkDeviceInfo::Record>& vk_device_records,
                                 Core::System& system_, bool enable_web_config)
    : QDialog(parent), ui{std::make_unique<Ui::ConfigureDialog>()}, registry(registry_),
      system{system_},
      builder{std::make_unique<ConfigurationShared::Builder>(this, !system_.IsPoweredOn())},
      applets_tab{std::make_unique<ConfigureApplets>(system_, nullptr, *builder, this)},
      audio_tab{std::make_unique<ConfigureAudio>(system_, nullptr, *builder, this)},
      cpu_tab{std::make_unique<ConfigureCpu>(system_, nullptr, *builder, this)},
      debug_tab_tab{std::make_unique<ConfigureDebugTab>(system_, this)},
      filesystem_tab{std::make_unique<ConfigureFilesystem>(this)},
      general_tab{std::make_unique<ConfigureGeneral>(system_, nullptr, *builder, this)},
      graphics_advanced_tab{
          std::make_unique<ConfigureGraphicsAdvanced>(system_, nullptr, *builder, this)},
      graphics_extensions_tab{
          std::make_unique<ConfigureGraphicsExtensions>(system_, nullptr, *builder, this)},
      ui_tab{std::make_unique<ConfigureUi>(system_, this)},
      graphics_tab{std::make_unique<ConfigureGraphics>(
          system_, vk_device_records, [&]() { graphics_advanced_tab->ExposeComputeOption(); },
          [this](Settings::AspectRatio ratio, Settings::ResolutionSetup setup) {
              ui_tab->UpdateScreenshotInfo(ratio, setup);
          },
          nullptr, *builder, this)},
      hotkeys_tab{std::make_unique<ConfigureHotkeys>(system_.HIDCore(), this)},
      input_tab{std::make_unique<ConfigureInput>(system_, this)},
      network_tab{std::make_unique<ConfigureNetwork>(system_, this)},
      profile_tab{std::make_unique<ConfigureProfileManager>(system_, this)},
      system_tab{std::make_unique<ConfigureSystem>(system_, nullptr, *builder, this)},
      web_tab{std::make_unique<ConfigureWeb>(this)} {
    Settings::SetConfiguringGlobal(true);

    ui->setupUi(this);

    // Eden-NS2: settings-list tabs scroll instead of growing the dialog past the screen.
    // Hotkeys/Profiles/Controls keep their own views. Tabs are added through PageFor().
    for (QWidget* const tab :
         {static_cast<QWidget*>(applets_tab.get()), static_cast<QWidget*>(audio_tab.get()),
          static_cast<QWidget*>(cpu_tab.get()), static_cast<QWidget*>(debug_tab_tab.get()),
          static_cast<QWidget*>(filesystem_tab.get()), static_cast<QWidget*>(general_tab.get()),
          static_cast<QWidget*>(graphics_tab.get()),
          static_cast<QWidget*>(graphics_advanced_tab.get()),
          static_cast<QWidget*>(graphics_extensions_tab.get()),
          static_cast<QWidget*>(network_tab.get()), static_cast<QWidget*>(system_tab.get()),
          static_cast<QWidget*>(ui_tab.get()), static_cast<QWidget*>(web_tab.get())}) {
        WrapTabInScrollArea(tab, this);
    }

    ui->tabWidget->addTab(PageFor(applets_tab.get()), tr("Applets"));
    ui->tabWidget->addTab(PageFor(audio_tab.get()), tr("Audio"));
    ui->tabWidget->addTab(PageFor(cpu_tab.get()), tr("CPU"));
    ui->tabWidget->addTab(PageFor(debug_tab_tab.get()), tr("Debug"));
    ui->tabWidget->addTab(PageFor(filesystem_tab.get()), tr("Filesystem"));
    ui->tabWidget->addTab(PageFor(general_tab.get()), tr("General"));
    ui->tabWidget->addTab(PageFor(graphics_tab.get()), tr("Graphics"));
    ui->tabWidget->addTab(PageFor(graphics_advanced_tab.get()), tr("GraphicsAdvanced"));
    ui->tabWidget->addTab(PageFor(graphics_extensions_tab.get()), tr("GraphicsExtra"));
    ui->tabWidget->addTab(hotkeys_tab.get(), tr("Hotkeys"));
    ui->tabWidget->addTab(input_tab.get(), tr("Controls"));
    ui->tabWidget->addTab(profile_tab.get(), tr("Profiles"));
    ui->tabWidget->addTab(PageFor(network_tab.get()), tr("Network"));
    ui->tabWidget->addTab(PageFor(system_tab.get()), tr("System"));
    ui->tabWidget->addTab(PageFor(ui_tab.get()), tr("Game List"));
    ui->tabWidget->addTab(PageFor(web_tab.get()), tr("Web"));

    web_tab->SetWebServiceConfigEnabled(enable_web_config);
    hotkeys_tab->Populate(registry);

    input_tab->Initialize(input_subsystem);

    general_tab->SetResetCallback([&] { this->close(); });

    SetConfiguration();
    PopulateSelectionList();

    connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int index) {
        if (index != -1) {
            debug_tab_tab->SetCurrentIndex(0);
        }
    });
    connect(ui_tab.get(), &ConfigureUi::LanguageChanged, this, &ConfigureDialog::OnLanguageChanged);
    connect(general_tab.get(), &ConfigureGeneral::ExternalContentDirsChanged, this,
            &ConfigureDialog::ExternalContentDirsChanged);
    connect(ui->selectorList, &QListWidget::itemSelectionChanged, this,
            &ConfigureDialog::UpdateVisibleTabs);

    if (system.IsPoweredOn()) {
        QPushButton* apply_button = ui->buttonBox->addButton(QDialogButtonBox::Apply);
        connect(apply_button, &QAbstractButton::clicked, this,
                &ConfigureDialog::HandleApplyButtonClicked);
    }

    adjustSize();
    ui->selectorList->setCurrentRow(0);

    // Eden-NS2: resizable, and never opens taller/wider than ~90% of the screen.
    setSizeGripEnabled(true);
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    FitDialogToScreen(this, QSize{std::max(width(), 650), std::max(height(), 700)});

    // Selects the leftmost button on the bottom bar (Cancel as of writing)
    ui->buttonBox->setFocus();
}

ConfigureDialog::~ConfigureDialog() = default;

void ConfigureDialog::SetConfiguration() {}

void ConfigureDialog::ApplyConfiguration() {
    general_tab->ApplyConfiguration();
    ui_tab->ApplyConfiguration();
    system_tab->ApplyConfiguration();
    profile_tab->ApplyConfiguration();
    filesystem_tab->ApplyConfiguration();
    input_tab->ApplyConfiguration();
    hotkeys_tab->ApplyConfiguration(registry);
    cpu_tab->ApplyConfiguration();
    graphics_tab->ApplyConfiguration();
    graphics_advanced_tab->ApplyConfiguration();
    graphics_extensions_tab->ApplyConfiguration();
    audio_tab->ApplyConfiguration();
    debug_tab_tab->ApplyConfiguration();
    web_tab->ApplyConfiguration();
    network_tab->ApplyConfiguration();
    applets_tab->ApplyConfiguration();
    system.ApplySettings();
    Settings::LogSettings();
}

void ConfigureDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

void ConfigureDialog::RetranslateUI() {
    const int old_row = ui->selectorList->currentRow();
    const int old_index = ui->tabWidget->currentIndex();

    ui->retranslateUi(this);

    PopulateSelectionList();
    ui->selectorList->setCurrentRow(old_row);

    UpdateVisibleTabs();
    ui->tabWidget->setCurrentIndex(old_index);
}

void ConfigureDialog::HandleApplyButtonClicked() {
    UISettings::values.configuration_applied = true;
    ApplyConfiguration();
}

Q_DECLARE_METATYPE(QList<QWidget*>);

void ConfigureDialog::PopulateSelectionList() {
    const std::array<std::pair<QString, QList<QWidget*>>, 6> items{
        {{tr("General"),
          {general_tab.get(), hotkeys_tab.get(), ui_tab.get(), web_tab.get(), debug_tab_tab.get()}},
         {tr("System"),
          {system_tab.get(), profile_tab.get(), network_tab.get(), filesystem_tab.get(),
           applets_tab.get()}},
         {tr("CPU"), {cpu_tab.get()}},
         {tr("Graphics"),
          {graphics_tab.get(), graphics_advanced_tab.get(), graphics_extensions_tab.get()}},
         {tr("Audio"), {audio_tab.get()}},
         {tr("Controls"), input_tab->GetSubTabs()}},
    };

    [[maybe_unused]] const QSignalBlocker blocker(ui->selectorList);

    ui->selectorList->clear();
    for (const auto& entry : items) {
        auto* const item = new QListWidgetItem(entry.first);
        item->setData(Qt::UserRole, QVariant::fromValue(entry.second));

        ui->selectorList->addItem(item);
    }
}

void ConfigureDialog::OnLanguageChanged(const QString& locale) {
    emit LanguageChanged(locale);
    //  Reloading the game list is needed to force retranslation.
    UISettings::values.is_game_list_reload_pending = true;
    // first apply the configuration, and then restore the display
    ApplyConfiguration();
    RetranslateUI();
    SetConfiguration();
}

void ConfigureDialog::UpdateVisibleTabs() {
    const auto items = ui->selectorList->selectedItems();
    if (items.isEmpty()) {
        return;
    }

    [[maybe_unused]] const QSignalBlocker blocker(ui->tabWidget);

    ui->tabWidget->clear();

    const auto tabs = qvariant_cast<QList<QWidget*>>(items[0]->data(Qt::UserRole));

    for (auto* const tab : tabs) {
        LOG_DEBUG(Frontend, "{}", tab->accessibleName().toStdString());
        ui->tabWidget->addTab(PageFor(tab), tab->accessibleName());
    }
}
