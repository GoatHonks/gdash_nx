/* config.c -- simple configuration parser
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <switch.h>

#include "config.h"

#define CONFIG_VARS \
  CONFIG_VAR_INT(screen_width); \
  CONFIG_VAR_INT(screen_height); \
  CONFIG_VAR_INT(cursor_speed); \
  CONFIG_VAR_INT(click_zone_x); \
  CONFIG_VAR_INT(click_zone_y); \
  CONFIG_VAR_INT(left_zone_x); \
  CONFIG_VAR_INT(left_zone_y); \
  CONFIG_VAR_INT(right_zone_x); \
  CONFIG_VAR_INT(right_zone_y); \
  CONFIG_VAR_INT(show_zones); \
  CONFIG_VAR_STR(click_buttons); \
  CONFIG_VAR_STR(left_buttons); \
  CONFIG_VAR_STR(right_buttons); \
  CONFIG_VAR_STR(back_buttons);

Config config;

// actual screen size that is in use right now
int screen_width = 1280;
int screen_height = 720;

static inline void parse_var(const char *name, const char *value) {
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { snprintf(config.var, sizeof(config.var), "%s", value); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_STR
}

static void set_defaults(Config *c) {
  memset(c, 0, sizeof(*c));
  c->screen_width = -1;  // auto (picks 720p handheld / 1080p docked)
  c->screen_height = -1;
  c->cursor_speed = 900; // stick cursor speed, px/s at 720p
  // Where a click lands with the cursor hidden. In a level ANY tap jumps, so
  // this position is free to aim at something useful in menus: the level box
  // on the level-select screen, centre (640, 225) on a 1280x720 capture.
  c->click_zone_x = 50;
  c->click_zone_y = 31;
  // The level-select prev/next arrows, measured off a 1280x720 capture:
  // centres at (77, 376) and (1219, 376). Kept as percentages so they hold
  // in docked 1080p too. The old 10/88 and 23/88 came from the Vita port's
  // PLATFORMER arrows, which sit somewhere else entirely -- if you play
  // platformer levels, aim these at those instead (see show_zones).
  c->left_zone_x  = 6;
  c->left_zone_y  = 52;
  c->right_zone_x = 95;
  c->right_zone_y = 52;
  c->show_zones   = 0;
  snprintf(c->click_buttons, sizeof(c->click_buttons), "A,ZR,R,ZL,L");
  snprintf(c->left_buttons, sizeof(c->left_buttons), "Left");
  snprintf(c->right_buttons, sizeof(c->right_buttons), "Right");
  snprintf(c->back_buttons, sizeof(c->back_buttons), "B,Plus");
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  set_defaults(&config);

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue; // skip comments
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      if (*tmp != 0) {
        *tmp = 0;
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; isspace((int)*tmp); --tmp) *tmp = 0;
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  return 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  fprintf(f,
    "# gdash_nx configuration\n"
    "\n"
    "# Render size. -1 = auto: 720p handheld, 1080p docked.\n"
    "screen_width %d\n"
    "screen_height %d\n"
    "\n"
    "# Speed of the stick-driven cursor, in px/s at 720p.\n"
    "cursor_speed %d\n",
    config.screen_width, config.screen_height, config.cursor_speed);

  fprintf(f,
    "\n"
    "# Buttons. Comma-separated, or none to disable. Valid names:\n"
    "#   A B X Y L R ZL ZR Plus Minus Up Down Left Right LStick RStick\n"
    "\n"
    "# Tap/click. Lands on the cursor while it is on screen, otherwise on\n"
    "# click_zone below. In a level any tap jumps, so aim only matters in menus.\n"
    "click_buttons %s\n"
    "\n"
    "# Tap the level-select previous/next arrows (see left_zone / right_zone).\n"
    "left_buttons %s\n"
    "right_buttons %s\n"
    "\n"
    "# Android BACK: pause in a level, go back in a menu.\n"
    "back_buttons %s\n",
    config.click_buttons, config.left_buttons,
    config.right_buttons, config.back_buttons);

  fprintf(f,
    "\n"
    "# Where the synthesised taps land, in percent of the screen (0-100).\n"
    "\n"
    "# Used by click_buttons when the cursor is hidden. Aimed at the level box\n"
    "# on the level-select screen.\n"
    "click_zone_x %d\n"
    "click_zone_y %d\n"
    "\n"
    "# The level-select previous/next arrows. Platformer movement arrows sit\n"
    "# elsewhere on screen, so re-aim these if you play platformer levels.\n"
    "left_zone_x %d\n"
    "left_zone_y %d\n"
    "right_zone_x %d\n"
    "right_zone_y %d\n"
    "\n"
    "# Draw a marker at each tap position so they can be aimed. 1 = on.\n"
    "show_zones %d\n",
    config.click_zone_x, config.click_zone_y,
    config.left_zone_x, config.left_zone_y,
    config.right_zone_x, config.right_zone_y,
    config.show_zones);

  fclose(f);

  return 0;
}

// Two-letter code for Cocos2dxHelper.getCurrentLanguage; the game only
// switches its few localized strings on it. Follows the console language.
const char *config_lang_iso2(void) {
  u64 lcode = 0;
  SetLanguage sl = SetLanguage_ENUS;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&lcode)))
      setMakeLanguage(lcode, &sl);
    setExit();
  }
  switch (sl) {
    case SetLanguage_FR:
    case SetLanguage_FRCA:  return "fr";
    case SetLanguage_DE:    return "de";
    case SetLanguage_IT:    return "it";
    case SetLanguage_ES:
    case SetLanguage_ES419: return "es";
    case SetLanguage_NL:    return "nl";
    case SetLanguage_PT:
    case SetLanguage_PTBR:  return "pt";
    case SetLanguage_RU:    return "ru";
    case SetLanguage_JA:    return "ja";
    case SetLanguage_KO:    return "ko";
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS:
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT:return "zh";
    default:                return "en";
  }
}
