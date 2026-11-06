#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include "ui/MacroPopup.hpp"

using namespace geode::prelude;

// Pause-menu button that opens the DynamicButter popup (macro mode, macro files, settings).
// It lives in its own menu instead of one of GD's: the node-ids mod that names GD's pause menus
// is disabled on 2.2091, and an own menu can't be broken by layout changes anyway.

class $modify(DBPauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();

		auto sprite = ButtonSprite::create("DB", "bigFont.fnt", "GJ_button_04.png", 0.6f);
		auto button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(DBPauseLayer::onDynamicButter));
		button->setID("settings-button"_spr);

		auto menu = CCMenu::create();
		menu->setID("menu"_spr);
		menu->addChild(button);
		auto win = CCDirector::get()->getWinSize();
		menu->setPosition({win.width - 30.f, 30.f}); // bottom-right corner
		this->addChild(menu, 10);
	}

	void onDynamicButter(CCObject*) {
		db::ui::openMacroPopup();
	}
};
