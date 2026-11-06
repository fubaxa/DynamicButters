#include "ui/MacroPopup.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>

#include "macro/MacroManager.hpp"
#include "macro/MacroStore.hpp"
#include "safety/SafeMode.hpp"
#include "macro/Seek.hpp"

using namespace geode::prelude;

namespace db::ui {
	namespace {
		char const* modeText(MacroMode m) {
			switch (m) {
				case MacroMode::Record: return "Record";
				case MacroMode::Playback: return "Playback";
				default: return "Off";
			}
		}

		std::string dateText(int64_t unix) {
			if (unix <= 0) return "unknown date";
			std::time_t t = static_cast<std::time_t>(unix);
			std::tm tm{};
			if (localtime_s(&tm, &t) != 0) return "unknown date";
			char buf[32];
			std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
			return buf;
		}

		std::string seconds(uint32_t tick) {
			return fmt::format("{:.1f} s", tick / 480.0);
		}

		CCLabelBMFont* label(std::string const& text, char const* font, float scale) {
			auto l = CCLabelBMFont::create(text.c_str(), font);
			l->setScale(scale);
			return l;
		}
	}

	// Minimal popup base on GD's FLAlertLayer (same setup as geode::Popup).
	// geode::Popup isn't used: the installed Geode 6.0.0-alpha.1 runtime exports different
	// signatures for its init and title functions than the SDK headers declare, and the mod then
	// fails to load with ERROR_PROC_NOT_FOUND.
	class DBPopup : public FLAlertLayer {
	protected:
		CCSize m_size;

		~DBPopup() override {
			CCTouchDispatcher::get()->unregisterForcePrio(this);
		}

		bool initPopup(float width, float height, char const* title) {
			m_size = CCSize{width, height};
			CCTouchDispatcher::get()->registerForcePrio(this, 2, 0);
			if (!this->initWithColor({0, 0, 0, 105})) return false;
			m_noElasticity = GameManager::get()->getGameVariable(GameVar::FastMenu);

			auto win = CCDirector::get()->getWinSize();
			m_mainLayer = CCLayer::create();
			m_mainLayer->ignoreAnchorPointForPosition(false);
			m_mainLayer->setContentSize(m_size);
			m_mainLayer->setPosition(win / 2);
			this->addChild(m_mainLayer);

			auto bg = extension::CCScale9Sprite::create("GJ_square01.png", {0.f, 0.f, 80.f, 80.f});
			bg->setContentSize(m_size);
			bg->setPosition(m_size / 2);
			m_mainLayer->addChild(bg);

			m_buttonMenu = CCMenu::create();
			m_buttonMenu->ignoreAnchorPointForPosition(false);
			m_buttonMenu->setContentSize(m_size);
			m_buttonMenu->setPosition(m_size / 2);
			m_buttonMenu->setZOrder(100);
			m_mainLayer->addChild(m_buttonMenu);

			auto closeSpr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
			closeSpr->setScale(0.8f);
			auto closeBtn = CCMenuItemExt::createSpriteExtra(closeSpr, [this](CCMenuItemSpriteExtra*) {
				this->onClose(nullptr);
			});
			m_buttonMenu->addChildAtPosition(closeBtn, Anchor::TopLeft, {3.f, -3.f});

			auto titleLabel = CCLabelBMFont::create(title, "goldFont.fnt");
			titleLabel->setZOrder(2);
			limitNodeWidth(titleLabel, m_size.width - 20.f, 0.7f, 0.1f);
			m_mainLayer->addChildAtPosition(titleLabel, Anchor::Top, {0.f, -20.f});

			this->setKeypadEnabled(true);
			this->setTouchEnabled(true);
			return true;
		}

		void registerWithTouchDispatcher() override {
			CCTouchDispatcher::get()->addTargetedDelegate(this, -500, true);
		}

		void keyBackClicked() override {
			this->onClose(nullptr);
		}

		void keyDown(enumKeyCodes key, double p1) override {
			if (key == KEY_Space) return;
			FLAlertLayer::keyDown(key, p1);
		}

	public:
		void onClose(CCObject*) {
			this->setKeypadEnabled(false);
			this->setTouchEnabled(false);
			this->removeFromParent();
		}
	};

	// ------------------------------------------------------------ file list

	class MacroListPopup : public DBPopup {
	public:
		static MacroListPopup* create(geode::Function<void()> onPicked) {
			auto ret = new MacroListPopup();
			ret->m_onPicked = std::move(onPicked);
			if (ret->initList()) {
				ret->autorelease();
				return ret;
			}
			delete ret;
			return nullptr;
		}

	protected:
		geode::Function<void()> m_onPicked;

		bool initList() {
			constexpr float W = 420.f, H = 270.f;
			if (!initPopup(W, H, "Macros")) return false;

			auto& mgr = MacroManager::get();
			auto const& cur = mgr.macro(); // level info of the open level
			auto entries = store::list();
			// This level's macros first (each group stays newest first).
			std::stable_partition(entries.begin(), entries.end(), [&](store::Entry const& e) {
				return e.macro.isFor(cur.levelID, cur.levelName);
			});

			constexpr float LIST_W = W - 40.f, LIST_H = H - 80.f, ROW_H = 36.f;
			auto scroll = ScrollLayer::create({LIST_W, LIST_H});
			scroll->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(3.f));

			for (auto const& e : entries) {
				bool here = e.macro.isFor(cur.levelID, cur.levelName);
				bool selected = e.path == mgr.selectedPath();

				auto row = CCNode::create();
				row->setContentSize({LIST_W, ROW_H});

				auto bg = CCLayerColor::create(here ? ccColor4B{60, 140, 60, 90} : ccColor4B{0, 0, 0, 70}, LIST_W, ROW_H);
				row->addChild(bg);

				auto name = label(e.fileName, "bigFont.fnt", 0.4f);
				name->setAnchorPoint({0.f, 0.5f});
				name->setPosition({8.f, ROW_H * 0.68f});
				limitNodeWidth(name, LIST_W - 90.f, 0.4f, 0.1f);
				if (selected) name->setColor({120, 255, 120});
				row->addChild(name);

				auto info = label(fmt::format("{} (ID {})  |  {} inputs  |  {}  |  {}  |  {}",
					e.macro.levelName, e.macro.levelID, e.macro.inputs.size(),
					e.macro.endFrame ? seconds(e.macro.endFrame) : "length ?",
					e.macro.botName.empty() ? "unknown bot" : e.macro.botName,
					dateText(e.macro.recordedAt)), "chatFont.fnt", 0.5f);
				info->setAnchorPoint({0.f, 0.5f});
				info->setPosition({8.f, ROW_H * 0.27f});
				info->setColor({200, 200, 200});
				limitNodeWidth(info, LIST_W - 90.f, 0.5f, 0.1f);
				row->addChild(info);

				auto menu = CCMenu::create();
				menu->setContentSize({70.f, ROW_H});
				menu->setPosition({LIST_W - 40.f, ROW_H / 2.f});
				menu->ignoreAnchorPointForPosition(false);
				auto path = e.path;
				auto btn = CCMenuItemExt::createSpriteExtra(
					ButtonSprite::create(selected ? "Using" : "Use", "bigFont.fnt",
						selected ? "GJ_button_01.png" : "GJ_button_04.png", 0.5f),
					[this, path](CCMenuItemSpriteExtra*) {
						MacroManager::get().selectFile(path);
						if (m_onPicked) m_onPicked();
						this->onClose(nullptr);
					});
				btn->setPosition({35.f, ROW_H / 2.f});
				menu->addChild(btn);
				row->addChild(menu);

				scroll->m_contentLayer->addChild(row);
			}
			scroll->m_contentLayer->updateLayout();
			scroll->scrollToTop();
			m_mainLayer->addChildAtPosition(scroll, Anchor::Center, {-LIST_W / 2.f, -LIST_H / 2.f - 8.f});

			if (entries.empty()) {
				auto empty = label("No .gdr2 files yet. Record a level, or put\nfiles from another bot in the macros folder.",
					"chatFont.fnt", 0.7f);
				empty->setAlignment(kCCTextAlignmentCenter);
				m_mainLayer->addChildAtPosition(empty, Anchor::Center);
			}
			return true;
		}
	};

	// ------------------------------------------------------------ macro start positions (M8)

	class StartPosPopup : public DBPopup {
	public:
		static StartPosPopup* create(geode::Function<void()> closeParent) {
			auto ret = new StartPosPopup();
			ret->m_closeParent = std::move(closeParent);
			if (ret->initStartPos()) {
				ret->autorelease();
				return ret;
			}
			delete ret;
			return nullptr;
		}

	protected:
		geode::Function<void()> m_closeParent;
		CCNode* m_listHolder = nullptr;
		CCLabelBMFont* m_info = nullptr;
		static constexpr float W = 400.f, H = 280.f, LIST_W = 360.f, LIST_H = 150.f, ROW_H = 26.f;
		static constexpr uint32_t TICKS_PER_SECOND = 480;

		bool initStartPos() {
			if (!initPopup(W, H, "Seek points")) return false;
			auto& mgr = MacroManager::get();
			mgr.ensurePlayMacro();

			m_info = label("", "chatFont.fnt", 0.6f);
			m_info->setAlignment(kCCTextAlignmentCenter);
			m_mainLayer->addChildAtPosition(m_info, Anchor::Top, {0.f, -45.f});

			m_listHolder = CCNode::create();
			m_mainLayer->addChild(m_listHolder);

			std::vector<CCNode*> row;
			auto addBtn = [&](char const* text, char const* bg, auto cb) {
				auto btn = CCMenuItemExt::createSpriteExtra(
					ButtonSprite::create(text, "bigFont.fnt", bg, 0.5f), std::move(cb));
				m_buttonMenu->addChild(btn);
				row.push_back(btn);
			};
			addBtn("Add current", "GJ_button_01.png", [this](CCMenuItemSpriteExtra*) { this->onAddCurrent(); });
			addBtn("Every 10 s", "GJ_button_04.png", [this](CCMenuItemSpriteExtra*) { this->onEvery(10); });
			addBtn("Clear", "GJ_button_06.png", [this](CCMenuItemSpriteExtra*) {
				MacroManager::get().clearStartTicks();
				this->rebuild();
			});
			// Same layout as the main popup's rows: centred, even gaps.
			float total = 0.f;
			for (auto n : row) total += n->getScaledContentSize().width;
			total += 10.f * (row.size() - 1);
			float x = (W - total) / 2.f;
			for (auto n : row) {
				float w = n->getScaledContentSize().width;
				n->setPosition({x + w / 2.f, 25.f});
				x += w + 10.f;
			}

			rebuild();
			return true;
		}

		void onAddCurrent() {
			auto& mgr = MacroManager::get();
			auto pl = PlayLayer::get();
			if (!pl || !mgr.playMacro()) return;
			uint32_t tick = pl->m_gameState.m_currentProgress;
			uint32_t free = mgr.playMacro()->latestFreeTick(tick);
			if (free == 0) {
				Notification::create("Can't add a seek point at the very start", NotificationIcon::Warning)->show();
				return;
			}
			if (free != tick) {
				Notification::create(fmt::format("Moved to tick {} (before a held click)", free), NotificationIcon::Info)->show();
			}
			mgr.addStartTick(free);
			rebuild();
		}

		void onEvery(unsigned secondsApart) {
			auto& mgr = MacroManager::get();
			auto const& play = mgr.playMacro();
			if (!play) return;
			uint32_t end = play->endFrame ? play->endFrame : play->lastInputFrame();
			for (uint32_t t = secondsApart * TICKS_PER_SECOND; t < end; t += secondsApart * TICKS_PER_SECOND) {
				if (uint32_t free = play->latestFreeTick(t); free > 0) mgr.addStartTick(free);
			}
			rebuild();
		}

		void rebuild() {
			m_listHolder->removeAllChildren();
			auto& mgr = MacroManager::get();
			auto const& play = mgr.playMacro();
			if (!play) {
				m_info->setString("No macro for this level.\nRecord one, or pick one with Load.");
				return;
			}
			m_info->setString(fmt::format("{} ({} inputs)\nGo: restarts in practice mode, fast-forwards the macro\nand starts you at that point.",
				mgr.playSource(), play->inputs.size()).c_str());
			limitNodeWidth(m_info, W - 30.f, 0.6f, 0.2f);

			auto scroll = ScrollLayer::create({LIST_W, LIST_H});
			scroll->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(2.f));
			auto const& ticks = play->startTicks;
			for (size_t i = 0; i < ticks.size(); ++i) {
				uint32_t tick = ticks[i];
				auto row = CCNode::create();
				row->setContentSize({LIST_W, ROW_H});
				row->addChild(CCLayerColor::create({0, 0, 0, 70}, LIST_W, ROW_H));

				auto text = label(fmt::format("#{}   {:.2f} s   (tick {})", i + 1, tick / double(TICKS_PER_SECOND), tick),
					"bigFont.fnt", 0.35f);
				text->setAnchorPoint({0.f, 0.5f});
				text->setPosition({8.f, ROW_H / 2.f});
				row->addChild(text);

				auto menu = CCMenu::create();
				menu->ignoreAnchorPointForPosition(false);
				menu->setContentSize({LIST_W, ROW_H});
				menu->setPosition({LIST_W / 2.f, ROW_H / 2.f});
				auto go = CCMenuItemExt::createSpriteExtra(
					ButtonSprite::create("Go", "bigFont.fnt", "GJ_button_01.png", 0.45f),
					[this, tick](CCMenuItemSpriteExtra*) {
						if (!seek::start(tick)) return;
						if (m_closeParent) m_closeParent();
						this->onClose(nullptr);
					});
				go->setPosition({LIST_W - 75.f, ROW_H / 2.f});
				menu->addChild(go);
				auto del = CCMenuItemExt::createSpriteExtra(
					ButtonSprite::create("X", "bigFont.fnt", "GJ_button_06.png", 0.45f),
					[this, tick](CCMenuItemSpriteExtra*) {
						MacroManager::get().removeStartTick(tick);
						this->rebuild();
					});
				del->setPosition({LIST_W - 25.f, ROW_H / 2.f});
				menu->addChild(del);
				row->addChild(menu);
				scroll->m_contentLayer->addChild(row);
			}
			scroll->m_contentLayer->updateLayout();
			scroll->scrollToTop();
			scroll->setPosition({(W - LIST_W) / 2.f, 48.f});
			m_listHolder->addChild(scroll);

			if (ticks.empty()) {
				auto empty = label("No seek points yet.\nPause where you want one and press Add current,\nor add one every 10 seconds.",
					"chatFont.fnt", 0.6f);
				empty->setAlignment(kCCTextAlignmentCenter);
				empty->setPosition({W / 2.f, 48.f + LIST_H / 2.f});
				m_listHolder->addChild(empty);
			}
		}
	};

	// ------------------------------------------------------------ main popup

	class MacroPopup : public DBPopup {
	public:
		static MacroPopup* create() {
			auto ret = new MacroPopup();
			if (ret->initMain()) {
				ret->autorelease();
				return ret;
			}
			delete ret;
			return nullptr;
		}

	protected:
		ButtonSprite* m_modeSprites[3] = {};
		CCLabelBMFont* m_status = nullptr;
		CCMenuItemSpriteExtra* m_autoBtn = nullptr;
		std::vector<CCNode*> m_bottomRow;
		CCMenuItemToggler* m_speedToggle = nullptr;
		Slider* m_speedSlider = nullptr;
		CCLabelBMFont* m_speedValue = nullptr;

		static constexpr float SPEED_MIN = 0.1f, SPEED_MAX = 2.0f, SPEED_STEP = 0.05f;

		// Places visible nodes side by side, centred on the popup, with `gap` between each pair.
		// m_mainLayer and m_buttonMenu have the same size and position, so x/y work for both.
		void layoutRow(std::vector<CCNode*> const& nodes, float y, float gap) {
			float total = 0.f;
			int count = 0;
			for (auto n : nodes) {
				if (!n->isVisible()) continue;
				total += n->getScaledContentSize().width;
				count++;
			}
			if (count == 0) return;
			total += gap * (count - 1);
			float x = (m_size.width - total) / 2.f;
			for (auto n : nodes) {
				if (!n->isVisible()) continue;
				float w = n->getScaledContentSize().width;
				// Position by the node's own anchor point.
				n->setPosition({x + w * n->getAnchorPoint().x, y});
				x += w + gap;
			}
		}

		static float speedFromSlider(float v) {
			float speed = SPEED_MIN + v * (SPEED_MAX - SPEED_MIN);
			return std::round(speed / SPEED_STEP) * SPEED_STEP;
		}

		static float sliderFromSpeed(float speed) {
			return std::clamp((speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN), 0.f, 1.f);
		}

		bool initMain() {
			constexpr float W = 430.f, H = 280.f;
			if (!initPopup(W, H, "DynamicButter")) return false;

			// Mode row: label + three buttons, evenly spaced
			float modeY = H - 58.f;
			auto modeLabel = label("Mode", "goldFont.fnt", 0.6f);
			m_mainLayer->addChild(modeLabel);
			std::vector<CCNode*> modeRow{modeLabel};
			MacroMode modes[3] = {MacroMode::Off, MacroMode::Record, MacroMode::Playback};
			for (int i = 0; i < 3; ++i) {
				auto mode = modes[i];
				auto spr = ButtonSprite::create(modeText(mode), "bigFont.fnt", "GJ_button_04.png", 0.6f);
				m_modeSprites[i] = spr;
				auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, mode](CCMenuItemSpriteExtra*) {
					MacroManager::get().setPendingMode(mode);
					this->refresh();
				});
				m_buttonMenu->addChild(btn);
				modeRow.push_back(btn);
			}
			layoutRow(modeRow, modeY, 10.f);

			// Speedhack row: label, on/off toggle, slider, value
			float speedY = H - 100.f;
			auto speedLabel = label("Speed", "goldFont.fnt", 0.6f);
			m_mainLayer->addChild(speedLabel);

			m_speedToggle = CCMenuItemExt::createTogglerWithStandardSprites(0.65f, [this](CCMenuItemToggler* t) {
				// GD flips the toggle after the callback, so the new state is the opposite of now.
				Mod::get()->setSettingValue<bool>("speedhack-enabled", !t->isToggled());
				this->refreshSpeed(!t->isToggled());
			});
			m_speedToggle->toggle(Mod::get()->getSettingValue<bool>("speedhack-enabled"));
			m_buttonMenu->addChild(m_speedToggle);

			m_speedSlider = Slider::create(this, menu_selector(MacroPopup::onSpeedSlider), 0.7f);
			float speed = static_cast<float>(Mod::get()->getSettingValue<double>("speedhack"));
			m_speedSlider->setValue(sliderFromSpeed(speed));
			// Slider's content size is 0; give it the bar's width so layoutRow can place it.
			m_speedSlider->setContentSize({150.f, 20.f});
			m_speedSlider->setAnchorPoint({0.5f, 0.5f});
			m_mainLayer->addChild(m_speedSlider);

			m_speedValue = label("1.00x", "bigFont.fnt", 0.45f);
			m_mainLayer->addChild(m_speedValue);
			// Fixed width for the value so the row doesn't shift while dragging.
			auto valueBox = CCNode::create();
			valueBox->setContentSize({50.f, 20.f});
			valueBox->setAnchorPoint({0.5f, 0.5f});
			m_mainLayer->addChild(valueBox);
			layoutRow({speedLabel, m_speedToggle, m_speedSlider, valueBox}, speedY, 12.f);
			m_speedValue->setPosition(valueBox->getPosition());

			m_status = label("", "chatFont.fnt", 0.7f);
			m_status->setAlignment(kCCTextAlignmentCenter);
			m_mainLayer->addChildAtPosition(m_status, Anchor::Center, {0.f, -18.f});

			// Bottom row
			auto addBtn = [this](char const* text, char const* bg, auto cb) {
				auto btn = CCMenuItemExt::createSpriteExtra(
					ButtonSprite::create(text, "bigFont.fnt", bg, 0.5f), std::move(cb));
				m_buttonMenu->addChild(btn);
				m_bottomRow.push_back(btn);
				return btn;
			};
			addBtn("Load", "GJ_button_01.png", [this](CCMenuItemSpriteExtra*) {
				MacroListPopup::create([this] { this->refresh(); })->show();
			});
			m_autoBtn = addBtn("Auto", "GJ_button_04.png", [this](CCMenuItemSpriteExtra*) {
				MacroManager::get().clearSelection();
				this->refresh();
			});
			addBtn("Folder", "GJ_button_04.png", [](CCMenuItemSpriteExtra*) {
				auto dir = store::macrosDir();
				(void)file::createDirectoryAll(dir);
				file::openFolder(dir);
			});
			addBtn("Seek", "GJ_button_02.png", [this](CCMenuItemSpriteExtra*) {
				StartPosPopup::create([this] { this->onClose(nullptr); })->show();
			});
			addBtn("Settings", "GJ_button_04.png", [](CCMenuItemSpriteExtra*) {
				openSettingsPopup(Mod::get());
			});

			refreshSpeed(Mod::get()->getSettingValue<bool>("speedhack-enabled"));
			refresh();
			return true;
		}

		void onSpeedSlider(CCObject*) {
			float speed = speedFromSlider(m_speedSlider->getValue());
			Mod::get()->setSettingValue<double>("speedhack", speed);
			refreshSpeed(m_speedToggle->isToggled());
		}

		void refreshSpeed(bool enabled) {
			float speed = static_cast<float>(Mod::get()->getSettingValue<double>("speedhack"));
			m_speedValue->setString(fmt::format("{:.2f}x", speed).c_str());
			m_speedValue->setColor(enabled ? ccColor3B{255, 255, 255} : ccColor3B{130, 130, 130});
		}

		void refresh() {
			auto& mgr = MacroManager::get();
			auto pending = mgr.pendingMode();
			MacroMode modes[3] = {MacroMode::Off, MacroMode::Record, MacroMode::Playback};
			for (int i = 0; i < 3; ++i) {
				m_modeSprites[i]->updateBGImage(modes[i] == pending ? "GJ_button_01.png" : "GJ_button_04.png");
			}
			m_autoBtn->setVisible(!mgr.selectedPath().empty());
			layoutRow(m_bottomRow, 28.f, 10.f);

			auto const& cur = mgr.macro();
			std::string text = fmt::format("Level: {} (ID {})\n", cur.levelName, cur.levelID);
			text += fmt::format("Active mode: {}\n", modeText(mgr.mode()));

			if (mgr.isRecording()) {
				text += fmt::format("Recording: {} inputs this attempt\n", cur.inputs.size());
			} else if (mgr.mode() == MacroMode::Playback) {
				if (auto const& play = mgr.playMacro()) {
					text += fmt::format("Playing: {}\n{} inputs, {}\n", mgr.playSource(), play->inputs.size(),
						play->botName.empty() ? "unknown bot" : play->botName);
				} else {
					text += "Playing: nothing (no macro found for this level)\n";
				}
			}
			if (!mgr.selectedPath().empty()) {
				text += fmt::format("Selected: {}\n", store::fileNameOf(mgr.selectedPath()));
			} else if (pending == MacroMode::Playback) {
				text += "Selected: newest macro of this level (Auto)\n";
			}
			if (!mgr.lastSaved().empty()) text += fmt::format("Last saved: {}\n", mgr.lastSaved());
			if (!safe::enabled()) {
				text += "Safe mode: OFF\n";
			} else if (safe::unsafe()) {
				text += fmt::format("Safe mode: this attempt won't save ({})\n", safe::reason());
			}
			if (mgr.hasPendingChange()) {
				text += "\nChanges apply when you restart the level.";
			} else if (pending == MacroMode::Record) {
				text += "\nThe macro is saved when you complete the level.";
			}
			m_status->setString(text.c_str());
			limitNodeSize(m_status, {390.f, 100.f}, 0.7f, 0.3f);
		}
	};

	void openMacroPopup() {
		if (auto popup = MacroPopup::create()) popup->show();
	}
}
