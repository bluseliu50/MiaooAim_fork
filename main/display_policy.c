#include "display_policy.h"

#include <stdatomic.h>
#include "scheduler.h"
#include "display_mode.h"
#include "epd.h"

static atomic_bool s_manual_screen;
static atomic_bool s_quick_refresh;
static atomic_uint s_display_epoch;

static bool weather_page_is_active(void)
{
    return display_mode_active() == DISPLAY_MODE_WEATHER;
}

void display_policy_init(void)
{
    atomic_store(&s_manual_screen, false);
    atomic_store(&s_quick_refresh, false);
    atomic_store(&s_display_epoch, 0);
}

void display_policy_set_quick_refresh_active(bool active)
{
    atomic_store(&s_quick_refresh, active);
}

bool display_policy_quick_refresh_active(void)
{
    return atomic_load(&s_quick_refresh);
}

bool display_policy_slideshow_owns_display(void)
{
    return display_mode_active() == DISPLAY_MODE_SLIDESHOW;
}

bool display_policy_clock_may_auto_refresh(void)
{
    if (display_mode_active() != DISPLAY_MODE_CLOCK)
        return false;
    if (atomic_load(&s_manual_screen))
        return false;
    if (display_policy_slideshow_owns_display())
        return false;
    return true;
}

bool display_policy_calendar_may_midnight_refresh(void)
{
    if (display_mode_active() != DISPLAY_MODE_CALENDAR)
        return false;
    return true;
}

bool display_policy_weather_use_full_page(bool user_explicit)
{
    if (user_explicit)
        return true;
    if (atomic_load(&s_quick_refresh))
        return false;
    if (display_policy_slideshow_owns_display())
        return false;
    if (weather_page_is_active())
        return true;
    if (display_mode_active() == DISPLAY_MODE_CLOCK)
        return false;
    if (atomic_load(&s_manual_screen))
        return false;
    return true;
}

bool display_policy_weather_may_network_fetch(bool user_explicit)
{
    if (user_explicit)
        return true;
    if (atomic_load(&s_quick_refresh))
        return false;
    return !display_policy_slideshow_owns_display();
}

bool display_policy_weather_may_render_full_page(void)
{
    if (atomic_load(&s_quick_refresh))
        return false;
    if (display_policy_slideshow_owns_display())
        return false;
    return weather_page_is_active();
}

void display_policy_set_manual_screen_active(bool active)
{
    atomic_store(&s_manual_screen, active);
}

bool display_policy_manual_screen_active(void)
{
    return atomic_load(&s_manual_screen);
}

unsigned display_policy_begin_manual_display(void)
{
    atomic_store(&s_manual_screen, true);
    unsigned epoch = atomic_fetch_add(&s_display_epoch, 1) + 1;
    epd_request_full_refresh_next();
    scheduler_notify_manual_show();
    return epoch;
}

bool display_policy_epoch_is_current(unsigned epoch)
{
    return atomic_load(&s_display_epoch) == epoch;
}

void display_policy_bump_display_epoch(void)
{
    atomic_fetch_add(&s_display_epoch, 1);
    epd_request_full_refresh_next();
}

unsigned display_policy_display_epoch(void)
{
    return atomic_load(&s_display_epoch);
}
