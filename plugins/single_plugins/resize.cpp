#include <cmath>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/view.hpp>
#include <wayfire/core.hpp>
#include <wayfire/workspace-manager.hpp>
#include <linux/input.h>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/plugins/wobbly/wobbly-signal.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>

class wayfire_resize : public wf::plugin_interface_t
{
    wf::signal_connection_t resize_request, view_destroyed;
    wf::button_callback activate_binding;
    wf::button_callback activate_binding_keep_ratio;

    wayfire_view view;

    bool was_client_request, is_using_touch;
    bool keep_ratio = false;
    wf::point_t grab_start;
    wf::geometry_t grabbed_geometry;

    uint32_t edges;
    wf::option_wrapper_t<wf::buttonbinding_t> button{"resize/activate"};
    wf::option_wrapper_t<wf::buttonbinding_t> button_keep_ratio{"resize/activate_keep_ratio"};

  public:
    void init() override
    {
        grab_interface->name = "resize";
        grab_interface->capabilities =
            wf::CAPABILITY_GRAB_INPUT | wf::CAPABILITY_MANAGE_DESKTOP;

        activate_binding = [=] (auto)
        {
            return activate(false);
        };

        activate_binding_keep_ratio = [=] (auto)
        {
            return activate(true);
        };

        output->add_button(button, &activate_binding);
        output->add_button(button_keep_ratio, &activate_binding_keep_ratio);
        grab_interface->callbacks.pointer.button = [=] (uint32_t b, uint32_t state)
        {
            if ((state == WLR_BUTTON_RELEASED) && was_client_request &&
                (b == BTN_LEFT))
            {
                return input_pressed(state);
            }

            if (b != wf::buttonbinding_t(button).get_button() && b != wf::buttonbinding_t(button_keep_ratio).get_button())
            {
                return;
            }

            input_pressed(state);
        };

        grab_interface->callbacks.pointer.motion = [=] (int, int)
        {
            input_motion();
        };

        grab_interface->callbacks.touch.up = [=] (int32_t id)
        {
            if (id == 0)
            {
                input_pressed(WLR_BUTTON_RELEASED);
            }
        };

        grab_interface->callbacks.touch.motion = [=] (int32_t id, int32_t, int32_t)
        {
            if (id == 0)
            {
                input_motion();
            }
        };

        grab_interface->callbacks.cancel = [=] ()
        {
            input_pressed(WLR_BUTTON_RELEASED);
        };

        using namespace std::placeholders;
        resize_request.set_callback(std::bind(
            std::mem_fn(&wayfire_resize::resize_requested), this, _1));
        output->connect_signal("view-resize-request", &resize_request);

        view_destroyed.set_callback([=] (wf::signal_data_t *data)
        {
            if (get_signaled_view(data) == view)
            {
                view = nullptr;
                input_pressed(WLR_BUTTON_RELEASED);
            }
        });

        output->connect_signal("view-disappeared", &view_destroyed);
    }

    bool activate(bool keep) {
        auto view = wf::get_core().get_cursor_focus_view();
        if (view)
        {
            is_using_touch     = false;
            was_client_request = false;
            keep_ratio = keep;

            return initiate(view);
        }

        return false;
    }

    void resize_requested(wf::signal_data_t *data)
    {
        auto request = static_cast<wf::view_resize_request_signal*>(data);
        auto view    = get_signaled_view(data);

        if (!view)
        {
            return;
        }

        auto touch = wf::get_core().get_touch_position(0);
        if (!std::isnan(touch.x) && !std::isnan(touch.y))
        {
            is_using_touch = true;
        } else
        {
            is_using_touch = false;
        }

        was_client_request = true;
        initiate(view, request->edges);
    }

    /* Returns the currently used input coordinates in global compositor space */
    wf::point_t get_global_input_coords()
    {
        wf::pointf_t input;
        if (is_using_touch)
        {
            input = wf::get_core().get_touch_position(0);
        } else
        {
            input = wf::get_core().get_cursor_position();
        }

        return {(int)input.x, (int)input.y};
    }

    /* Returns the currently used input coordinates in output-local space */
    wf::point_t get_input_coords()
    {
        auto og = output->get_layout_geometry();

        return get_global_input_coords() - wf::point_t{og.x, og.y};
    }

    /* Calculate resize edges, grab starts at (sx, sy), view's geometry is vg */
    uint32_t calculate_edges(wf::geometry_t vg, int sx, int sy)
    {
        int view_x = sx - vg.x;
        int view_y = sy - vg.y;

        uint32_t edges = 0;
        if (view_x < vg.width / 2)
        {
            edges |= WLR_EDGE_LEFT;
        } else
        {
            edges |= WLR_EDGE_RIGHT;
        }

        if (view_y < vg.height / 2)
        {
            edges |= WLR_EDGE_TOP;
        } else
        {
            edges |= WLR_EDGE_BOTTOM;
        }

        return edges;
    }

    bool initiate(wayfire_view view, uint32_t forced_edges = 0)
    {
        if (!view || (view->role == wf::VIEW_ROLE_DESKTOP_ENVIRONMENT) ||
            !view->is_mapped() || view->fullscreen)
        {
            return false;
        }

        this->edges = forced_edges ?: calculate_edges(view->get_bounding_box(),
            get_input_coords().x, get_input_coords().y);

        if (edges == 0)
        {
            return false;
        }

        auto current_ws_impl =
            output->workspace->get_workspace_implementation();
        if (!current_ws_impl->view_resizable(view))
        {
            return false;
        }

        if (!output->activate_plugin(grab_interface))
        {
            return false;
        }

        if (!grab_interface->grab())
        {
            output->deactivate_plugin(grab_interface);

            return false;
        }

        grab_start = get_input_coords();
        grabbed_geometry = view->get_wm_geometry();

        if ((edges & WLR_EDGE_LEFT) || (edges & WLR_EDGE_TOP))
        {
            view->set_moving(true);
        }

        view->set_resizing(true, edges);

        if (view->tiled_edges)
        {
            view->set_tiled(0);
        }

        this->view = view;

        auto og = view->get_bounding_box();
        int anchor_x = og.x;
        int anchor_y = og.y;

        if (edges & WLR_EDGE_LEFT)
        {
            anchor_x += og.width;
        }

        if (edges & WLR_EDGE_TOP)
        {
            anchor_y += og.height;
        }

        start_wobbly(view, anchor_x, anchor_y);
        wf::get_core().set_cursor(wlr_xcursor_get_resize_name((wlr_edges)edges));

        return true;
    }

    void input_pressed(uint32_t state)
    {
        if (state != WLR_BUTTON_RELEASED)
        {
            return;
        }

        grab_interface->ungrab();
        output->deactivate_plugin(grab_interface);

        if (view)
        {
            if ((edges & WLR_EDGE_LEFT) ||
                (edges & WLR_EDGE_TOP))
            {
                view->set_moving(false);
            }

            view->set_resizing(false);
            end_wobbly(view);

            wf::view_change_workspace_signal workspace_may_changed;
            workspace_may_changed.view = this->view;
            workspace_may_changed.to   = output->workspace->get_current_workspace();
            workspace_may_changed.old_workspace_valid = false;
            output->emit_signal("view-change-workspace", &workspace_may_changed);
        }
    }

    void input_motion()
    {
        auto input = get_input_coords();
        int dx     = input.x - grab_start.x;
        int dy     = input.y - grab_start.y;
        int width  = grabbed_geometry.width;
        int height = grabbed_geometry.height;
        double ratio;
        if (keep_ratio)
        {
          ratio = (double)width / height;
        }

        if (edges & WLR_EDGE_LEFT)
        {
            width -= dx;
        } else if (edges & WLR_EDGE_RIGHT)
        {
            width += dx;
        }

        if (edges & WLR_EDGE_TOP)
        {
            height -= dy;
        } else if (edges & WLR_EDGE_BOTTOM)
        {
            height += dy;
        }

        if (keep_ratio)
        {
          height = std::min(std::max(height, 1), (int)(width / ratio));
          width  = std::min(std::max(width, 1), (int)(height * ratio));
        } else {
          height = std::max(height, 1);
          width  = std::max(width, 1);
        }
        view->resize(width, height);
    }

    void fini() override
    {
        if (grab_interface->is_grabbed())
        {
            input_pressed(WLR_BUTTON_RELEASED);
        }

        output->rem_binding(&activate_binding);
        output->rem_binding(&activate_binding_keep_ratio);
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_resize);
