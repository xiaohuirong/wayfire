#include "pointer.hpp"
#include <wayfire/bindings-repository.hpp>
#include "cursor.hpp"
#include "pointing-device.hpp"
#include "input-manager.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/signal-definitions.hpp"

#include "../scene-priv.hpp"
#include <wayfire/debug.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/core.hpp>
#include <wayfire/output-layout.hpp>

wf::pointer_t::pointer_t(nonstd::observer_ptr<wf::input_manager_t> input,
    nonstd::observer_ptr<seat_t> seat)
{
    this->input = input;
    this->seat  = seat;

    on_root_node_updated = [=] (wf::scene::root_node_update_signal *data)
    {
        if (!(data->flags & wf::scene::update_flag::INPUT_STATE))
        {
            return;
        }

        if (grabbed_node && !is_grabbed_node_alive(grabbed_node))
        {
            grab_surface(nullptr);
        }

        update_cursor_position(get_current_time(), false);
    };

    wf::get_core().scene()->connect(&on_root_node_updated);
}

wf::pointer_t::~pointer_t()
{}

bool wf::pointer_t::has_pressed_buttons() const
{
    return this->count_pressed_buttons > 0;
}

/* ------------------------- Cursor focus functions ------------------------- */
void wf::pointer_t::set_enable_focus(bool enabled)
{
    this->focus_enabled_count += enabled ? 1 : -1;
    if (focus_enabled_count > 1)
    {
        LOGI("LogicalPointer enabled more times than disabled?");
    }

    // reset grab
    if (!focus_enabled())
    {
        grab_surface(nullptr);
        this->update_cursor_focus(nullptr);
    } else
    {
        update_cursor_position(get_current_time(), false);
    }
}

bool wf::pointer_t::focus_enabled() const
{
    return this->focus_enabled_count > 0;
}

void wf::pointer_t::update_cursor_position(int64_t time_msec, bool real_update)
{
    wf::pointf_t gc = seat->priv->cursor->get_cursor_position();

    /* If we have a grabbed surface, but no drag, we want to continue sending
     * events to the grabbed surface, even if the pointer goes outside of it.
     * This enables Xwayland DnD to work correctly, and also lets the user for
     * ex. grab a scrollbar and move their mouse freely. */
    if (!grabbed_node && this->focus_enabled())
    {
        const auto& scene = wf::get_core().scene();
        auto isec = scene->find_node_at(gc);
        update_cursor_focus(isec ? isec->node->shared_from_this() : nullptr);
    }

    if (real_update)
    {
        this->send_motion(time_msec);
    }

    seat->priv->update_drag_icon();
}

void wf::pointer_t::force_release_buttons()
{
    if (cursor_focus)
    {
        for (auto button : this->currently_sent_buttons)
        {
            wlr_pointer_button_event event;
            event.pointer   = NULL;
            event.button    = button;
            event.state     = WLR_BUTTON_RELEASED;
            event.time_msec = wf::get_current_time();
            cursor_focus->pointer_interaction().handle_pointer_button(event);
        }

        cursor_focus->pointer_interaction().handle_pointer_leave();
    }
}

void wf::pointer_t::transfer_grab(scene::node_ptr node, bool retain_pressed_state)
{
    if (node == cursor_focus)
    {
        LOGD("transfer grab ", cursor_focus.get(), " -> ", node.get(), ": do nothing");
        // Node might already be focused, in case for example there was no input surface when the grab node
        // was added to the scenegraph.
        return;
    }

    LOGD("transfer grab ", cursor_focus.get(), " -> ", node.get());
    force_release_buttons();
    cursor_focus = node;

    // Send pointer_enter to the grab
    auto gc    = wf::get_core().get_cursor_position();
    auto local = get_node_local_coords(node.get(), gc);
    node->pointer_interaction().handle_pointer_enter(local);

    if (!retain_pressed_state)
    {
        currently_sent_buttons.clear();
    }

    if (currently_sent_buttons.size())
    {
        grabbed_node = node;
    } else
    {
        grabbed_node = nullptr;
    }
}

void wf::pointer_t::update_cursor_focus(wf::scene::node_ptr new_focus)
{
    bool focus_change = (cursor_focus != new_focus);
    if (focus_change)
    {
        LOGD("change cursor focus ", cursor_focus.get(), " -> ", new_focus.get());
    }

    // Clear currently sent buttons when switching focus
    // However, if we are in drag-and-drop mode, do not release
    // buttons since otherwise we'll cancel DnD
    if (focus_change)
    {
        force_release_buttons();
        currently_sent_buttons.clear();
    }

    cursor_focus = new_focus;
    if (focus_change && new_focus)
    {
        auto gc    = wf::get_core().get_cursor_position();
        auto local = get_node_local_coords(new_focus.get(), gc);
        new_focus->pointer_interaction().handle_pointer_enter(local);
    } else if (focus_change)
    {
        // If there is focused surface, we should reset the cursor image to
        // avoid the last cursor image getting stuck outside of its surface.
        wf::get_core().set_cursor("default");
    }
}

wf::scene::node_ptr wf::pointer_t::get_focus() const
{
    return this->cursor_focus;
}

/* -------------------------- Implicit grab --------------------------------- */
void wf::pointer_t::grab_surface(wf::scene::node_ptr node)
{
    if (node == grabbed_node)
    {
        return;
    }

    if (node)
    {
        /* Start a new grab */
        this->grabbed_node = node;
        return;
    }

    /* End grab */
    grabbed_node = nullptr;
    update_cursor_position(get_current_time(), false);
}

/* ----------------------- Input event processing --------------------------- */
void wf::pointer_t::handle_pointer_button(wlr_pointer_button_event *ev,
    input_event_processing_mode_t mode)
{
    seat->priv->break_mod_bindings();
    bool handled_in_binding = (mode != input_event_processing_mode_t::FULL);

    if (ev->state == WLR_BUTTON_PRESSED)
    {
        count_pressed_buttons++;
        if (count_pressed_buttons == 1)
        {
            /* Focus only the first click, since then we also start an implicit
             * grab, and we don't want to suddenly change the output */
            auto gc     = seat->priv->cursor->get_cursor_position();
            auto output =
                wf::get_core().output_layout->get_output_at(gc.x, gc.y);
            wf::get_core().focus_output(output);
        }

        uint32_t modifiers = seat->priv->get_modifiers();
        auto view = seat->keyboard_focus.get();
        if (handled_in_binding || !modifiers ||
            !(view && view->keyboard_interaction().inhibitor_active()))
        {
          handled_in_binding |= wf::get_core().bindings->handle_button(
              wf::buttonbinding_t{modifiers, ev->button});
        }
    } else
    {
        count_pressed_buttons--;
    }

    send_button(ev, handled_in_binding);
    if (!handled_in_binding)
    {
        check_implicit_grab();
    }
}

void wf::pointer_t::check_implicit_grab()
{
    /* start a button held grab, so that the window will receive all the
     * subsequent events, no matter what happens */
    if ((count_pressed_buttons == 1) && cursor_focus)
    {
        grab_surface(cursor_focus);
    }

    /* end the button held grab. We need to to this here after we have send
     * the last button release event, so that buttons don't get stuck in clients */
    if (count_pressed_buttons == 0)
    {
        grab_surface(nullptr);
    }
}

void wf::pointer_t::send_button(wlr_pointer_button_event *ev, bool has_binding)
{
    /* Clients do not receive buttons for bindings */
    if (has_binding)
    {
        return;
    }

    if (cursor_focus)
    {
        if ((ev->state == WLR_BUTTON_PRESSED) && cursor_focus)
        {
            this->currently_sent_buttons.insert(ev->button);
            cursor_focus->pointer_interaction().handle_pointer_button(*ev);
        } else if ((ev->state == WLR_BUTTON_RELEASED) &&
                   currently_sent_buttons.count(ev->button))
        {
            this->currently_sent_buttons.erase(
                currently_sent_buttons.find(ev->button));
            cursor_focus->pointer_interaction().handle_pointer_button(*ev);
        } else
        {
            // Ignore buttons which the client has not received.
            // These are potentially buttons which were grabbed.
        }
    }
}

void wf::pointer_t::send_motion(uint32_t time_msec)
{
    if (cursor_focus)
    {
        auto gc    = wf::get_core().get_cursor_position();
        auto local = get_node_local_coords(cursor_focus.get(), gc);
        cursor_focus->pointer_interaction().handle_pointer_motion(local, time_msec);
    }
}

void wf::pointer_t::handle_pointer_motion(wlr_pointer_motion_event *ev,
    input_event_processing_mode_t mode)
{
    /* XXX: maybe warp directly? */
    wlr_cursor_move(seat->priv->cursor->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    update_cursor_position(ev->time_msec);
}

void wf::pointer_t::handle_pointer_motion_absolute(
    wlr_pointer_motion_absolute_event *ev, input_event_processing_mode_t mode)
{
    // next coordinates
    double cx, cy;
    wlr_cursor_absolute_to_layout_coords(seat->priv->cursor->cursor, &ev->pointer->base,
        ev->x, ev->y, &cx, &cy);

    // send relative motion
    double dx = cx - seat->priv->cursor->cursor->x;
    double dy = cy - seat->priv->cursor->cursor->y;
    wlr_relative_pointer_manager_v1_send_relative_motion(
        wf::get_core().protocols.relative_pointer, seat->seat,
        (uint64_t)ev->time_msec * 1000, dx, dy, dx, dy);

    // TODO: indirection via wf_cursor
    wlr_cursor_warp_closest(seat->priv->cursor->cursor, NULL, cx, cy);
    update_cursor_position(ev->time_msec);
}

void wf::pointer_t::handle_pointer_axis(wlr_pointer_axis_event *ev,
    input_event_processing_mode_t mode)
{
    uint32_t modifiers = seat->priv->get_modifiers();

    bool handled_in_binding = false;
    auto view = seat->keyboard_focus.get();
    if (!input->active_grab && modifiers &&
        (view && view->keyboard_interaction().inhibitor_active()))
    {
      // do not use for binding
    } else
    {
      handled_in_binding = wf::get_core().bindings->handle_axis(modifiers, ev);
    }
    seat->priv->break_mod_bindings();


    /* Do not send scroll events to clients if an axis binding has used up the
     * event */
    if (handled_in_binding)
    {
        return;
    }

    /* Calculate speed settings */
    double mult = ev->source == WLR_AXIS_SOURCE_FINGER ?
        wf::pointing_device_t::config.touchpad_scroll_speed :
        wf::pointing_device_t::config.mouse_scroll_speed;

    ev->delta *= mult;
    ev->delta_discrete *= mult;

    if (cursor_focus)
    {
        cursor_focus->pointer_interaction().handle_pointer_axis(*ev);
    }
}

void wf::pointer_t::handle_pointer_swipe_begin(wlr_pointer_swipe_begin_event *ev,
    input_event_processing_mode_t mode)
{
    wlr_pointer_gestures_v1_send_swipe_begin(
        wf::get_core().protocols.pointer_gestures, seat->seat,
        ev->time_msec, ev->fingers);
}

void wf::pointer_t::handle_pointer_swipe_update(
    wlr_pointer_swipe_update_event *ev, input_event_processing_mode_t mode)
{
    wlr_pointer_gestures_v1_send_swipe_update(
        wf::get_core().protocols.pointer_gestures, seat->seat,
        ev->time_msec, ev->dx, ev->dy);
}

void wf::pointer_t::handle_pointer_swipe_end(wlr_pointer_swipe_end_event *ev,
    input_event_processing_mode_t mode)
{
    wlr_pointer_gestures_v1_send_swipe_end(
        wf::get_core().protocols.pointer_gestures, seat->seat,
        ev->time_msec, ev->cancelled);
}

void wf::pointer_t::handle_pointer_pinch_begin(wlr_pointer_pinch_begin_event *ev,
    input_event_processing_mode_t mode)
{
    wlr_pointer_gestures_v1_send_pinch_begin(
        wf::get_core().protocols.pointer_gestures, seat->seat,
        ev->time_msec, ev->fingers);
}

void wf::pointer_t::handle_pointer_pinch_update(
    wlr_pointer_pinch_update_event *ev, input_event_processing_mode_t mode)
{
    wlr_pointer_gestures_v1_send_pinch_update(
        wf::get_core().protocols.pointer_gestures, seat->seat,
        ev->time_msec, ev->dx, ev->dy, ev->scale, ev->rotation);
}

void wf::pointer_t::handle_pointer_pinch_end(wlr_pointer_pinch_end_event *ev,
    input_event_processing_mode_t mode)
{
    wlr_pointer_gestures_v1_send_pinch_end(
        wf::get_core().protocols.pointer_gestures, seat->seat,
        ev->time_msec, ev->cancelled);
}

void wf::pointer_t::handle_pointer_hold_begin(wlr_pointer_hold_begin_event *ev,
    input_event_processing_mode_t mode)
{
    wlr_pointer_gestures_v1_send_hold_begin(
        wf::get_core().protocols.pointer_gestures, seat->seat,
        ev->time_msec, ev->fingers);
}

void wf::pointer_t::handle_pointer_hold_end(wlr_pointer_hold_end_event *ev,
    input_event_processing_mode_t mode)
{
    wlr_pointer_gestures_v1_send_hold_end(
        wf::get_core().protocols.pointer_gestures, seat->seat,
        ev->time_msec, ev->cancelled);
}

void wf::pointer_t::handle_pointer_frame()
{
    wlr_seat_pointer_notify_frame(seat->seat);
}
