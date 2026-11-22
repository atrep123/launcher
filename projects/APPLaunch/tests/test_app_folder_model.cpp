/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "desktop_entry.h"
#include "model/app_folder_model.hpp"
#include "model/launcher_navigation_model.hpp"

#include <cassert>
#include <vector>

namespace {

using App = AppFolderModel::App;

const std::vector<App> apps = {
    {"settings", AppFolder::System},
    {"rf-sonda", AppFolder::Radio},
    {"calculator", AppFolder::Tools},
    {"snake", AppFolder::Games},
    {"compass", AppFolder::Radio},
    {"untagged", AppFolder::Other},
};

AppFolder folder_from_desktop(const std::string &fields)
{
    const auto entry = parse_desktop_entry("[Desktop Entry]\nName=Sample\nExec=sample\n" + fields);
    assert(entry);
    return app_folder_for_categories(entry->categories);
}

void test_desktop_categories()
{
    assert(folder_from_desktop("Categories=Game;\n") == AppFolder::Games);
    assert(folder_from_desktop("Categories=Utility;HamRadio;\r\n") == AppFolder::Radio);
    assert(folder_from_desktop("Categories=HamRadio;Utility\n") == AppFolder::Radio);
    assert(folder_from_desktop("Categories=Settings;HardwareSettings;\n") == AppFolder::Radio);
    assert(folder_from_desktop("Categories=System;Monitor\n") == AppFolder::System);
    assert(folder_from_desktop("Categories=AudioVideo;Player;\n") == AppFolder::Media);
    assert(folder_from_desktop("Categories=Development;\n") == AppFolder::Tools);
    assert(folder_from_desktop("Categories=NotAGame;game;\n") == AppFolder::Other);
    assert(folder_from_desktop("Categories=;;;\n") == AppFolder::Other);
    assert(folder_from_desktop("[Desktop Action Run]\nCategories=Game;\n") == AppFolder::Other);
    assert(folder_from_desktop("") == AppFolder::Other);
}

void test_flat_carousel()
{
    AppFolderModel carousel;
    carousel.rebuild(apps, false, 3);
    assert(!carousel.enabled() && carousel.size() == apps.size() && carousel.index() == 2);
    assert(!carousel.open_folder() && !carousel.close_folder());
    for (std::size_t index : {2, 3, 4, 5, 0, 1, 2}) {
        const auto item = carousel.item();
        assert(item && !item->folder && item->app_index == index);
        carousel.move(1);
    }

    carousel.rebuild(apps, false, 3);
    assert(carousel.index() == 0);
    carousel.move(-1);
    assert(carousel.item()->app_index == 5);

    carousel.rebuild({apps[0]}, false);
    assert(carousel.item(-1)->app_index == 0);
    assert(carousel.item(1)->app_index == 0);
}

void test_folder_navigation()
{
    AppFolderModel carousel;
    carousel.rebuild(apps, false);
    carousel.rebuild(apps, true);
    assert(carousel.size() == 5 && carousel.item()->folder == AppFolder::System);
    assert(carousel.folder_size(AppFolder::Media) == 0);
    assert(carousel.open_folder());
    assert(carousel.size() == 1 && carousel.item()->app_index == 0);
    assert(!carousel.item(-1) && !carousel.item(1));
    assert(carousel.close_folder());
    assert(carousel.item()->folder == AppFolder::System);
    carousel.move(2);
    assert(carousel.item()->folder == AppFolder::Radio);
    assert(carousel.folder_size(AppFolder::Radio) == 2);
    assert(carousel.open_folder());
    assert(!carousel.open_folder());
    assert(carousel.item()->app_index == 1);
    carousel.move(1);
    assert(carousel.item()->app_index == 4);

    LauncherPageLifecycleModel lifecycle;
    assert(lifecycle.begin_app());
    assert(lifecycle.request_home());
    assert(lifecycle.complete_home());
    assert(carousel.index() == 1 && carousel.item()->app_index == 4);

    // A directory refresh may move the app while a page is open. Keep its
    // identity and folder, and leave the folder if its last app disappears.
    carousel.rebuild({apps[4], apps[0], apps[1]}, true);
    assert(carousel.index() == 0 && carousel.item()->app_index == 0);
    assert(carousel.close_folder());
    assert(carousel.item()->folder == AppFolder::Radio);
    assert(carousel.open_folder());
    carousel.rebuild({apps[0]}, true);
    assert(carousel.item()->folder == AppFolder::System);
    assert(!carousel.close_folder());

    carousel.rebuild(apps, false);
    assert(carousel.index() == 0 && carousel.item()->app_index == 0);
    carousel.rebuild({}, true);
    assert(carousel.size() == 0 && !carousel.item() && !carousel.open_folder());
}

} // namespace

int main()
{
    test_desktop_categories();
    test_flat_carousel();
    test_folder_navigation();
}
