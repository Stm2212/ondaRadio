#include <stdbool.h>

#include "radio_ui_lvgl.h"
#include "radio_test_adapters.h"

typedef enum {
    UI_WIDGET_OBJECT,
    UI_WIDGET_LABEL,
    UI_WIDGET_BUTTON,
    UI_WIDGET_BAR,
    UI_WIDGET_LINE,
} ui_widget_kind_t;

#ifdef RADIO_UI_TEST_FAULT_INJECTION
static size_t s_creation_attempts;

void radio_ui_lvgl_test_fail_on(size_t nth_creation)
{
    s_creation_attempts = 0;
    radio_test_adapter_fail_on(RADIO_TEST_OPERATION_ALLOCATION,
                               (uint32_t)nth_creation);
}

size_t radio_ui_lvgl_test_creation_attempts(void)
{
    return s_creation_attempts;
}

void radio_ui_lvgl_test_clear_fault(void)
{
    s_creation_attempts = 0;
    radio_test_adapter_clear_fault(RADIO_TEST_OPERATION_ALLOCATION);
}

static bool creation_should_fail(void)
{
    ++s_creation_attempts;
    return radio_test_adapter_should_fail(RADIO_TEST_OPERATION_ALLOCATION);
}
#else
static bool creation_should_fail(void)
{
    return false;
}
#endif

static lv_obj_t *widget_create(ui_widget_kind_t kind, lv_obj_t *parent)
{
    if (kind != UI_WIDGET_OBJECT && parent == NULL) return NULL;
    if (creation_should_fail()) return NULL;

    switch (kind) {
    case UI_WIDGET_OBJECT: return lv_obj_create(parent);
    case UI_WIDGET_LABEL: return lv_label_create(parent);
    case UI_WIDGET_BUTTON: return lv_button_create(parent);
    case UI_WIDGET_BAR: return lv_bar_create(parent);
    case UI_WIDGET_LINE: return lv_line_create(parent);
    default: return NULL;
    }
}

lv_obj_t *radio_ui_lvgl_obj_create(lv_obj_t *parent)
{
    return widget_create(UI_WIDGET_OBJECT, parent);
}

lv_obj_t *radio_ui_lvgl_label_create(lv_obj_t *parent)
{
    return widget_create(UI_WIDGET_LABEL, parent);
}

lv_obj_t *radio_ui_lvgl_button_create(lv_obj_t *parent)
{
    return widget_create(UI_WIDGET_BUTTON, parent);
}

lv_obj_t *radio_ui_lvgl_bar_create(lv_obj_t *parent)
{
    return widget_create(UI_WIDGET_BAR, parent);
}

lv_obj_t *radio_ui_lvgl_line_create(lv_obj_t *parent)
{
    return widget_create(UI_WIDGET_LINE, parent);
}
