#include "wayfire/plugins/common/input-grab.hpp"
#include "wayfire/scene-input.hpp"
#include <cmath>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/view.hpp>
#include <wayfire/core.hpp>
#include <wayfire/workspace-manager.hpp>
#include <linux/input.h>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/plugins/wobbly/wobbly-signal.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>

class wayfire_resize : public wf::per_output_plugin_instance_t, public wf::pointer_interaction_t,
    public wf::touch_interaction_t
{
    wf::signal::connection_t<wf::view_resize_request_signal> on_resize_request =
        [=] (wf::view_resize_request_signal *request)
    {
        if (!request->view)
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
        initiate(request->view, request->edges);
    };

    wf::signal::connection_t<wf::view_disappeared_signal> on_view_disappeared =
        [=] (wf::view_disappeared_signal *ev)
    {
        if (ev->view == view)
        {
            view = nullptr;
            input_pressed(WLR_BUTTON_RELEASED);
        }
    };

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
    std::unique_ptr<wf::input_grab_t> input_grab;
    wf::plugin_activation_data_t grab_interface = {
        .name = "resize",
        .capabilities = wf::CAPABILITY_GRAB_INPUT | wf::CAPABILITY_MANAGE_DESKTOP,
    };

  public:
    void init() override
    {
        input_grab = std::make_unique<wf::input_grab_t>("resize", output, nullptr, this, this);

        activate_binding = [=] (auto)
        {
            activate(false);
            return false;
        };

        activate_binding_keep_ratio = [=] (auto)
        {
            activate(true);
            return false;
        };

        output->add_button(button, &activate_binding);
        output->add_button(button_keep_ratio, &activate_binding_keep_ratio);
        grab_interface.cancel = [=] ()
        {
            input_pressed(WLR_BUTTON_RELEASED);
        };

        output->connect(&on_resize_request);
        output->connect(&on_view_disappeared);
    }

    void activate(bool keep) {
        auto view = wf::get_core().get_cursor_focus_view();
        if (view)
        {
            is_using_touch     = false;
            was_client_request = false;
            keep_ratio         = keep;
            initiate(view);
        }
    }

    void handle_pointer_button(const wlr_pointer_button_event& event) override
    {
        if ((event.state == WLR_BUTTON_RELEASED) && was_client_request && (event.button == BTN_LEFT))
        {
            return input_pressed(event.state);
        }

        if (event.button != wf::buttonbinding_t(button).get_button() && event.button != wf::buttonbinding_t(button_keep_ratio).get_button())
        {
            return;
        }

        input_pressed(event.state);
    }

    void handle_pointer_motion(wf::pointf_t pointer_position, uint32_t time_ms) override
    {
        input_motion();
    }

    void handle_touch_up(uint32_t time_ms, int finger_id, wf::pointf_t lift_off_position) override
    {
        if (finger_id == 0)
        {
            input_pressed(WLR_BUTTON_RELEASED);
        }
    }

    void handle_touch_motion(uint32_t time_ms, int finger_id, wf::pointf_t position) override
    {
        if (finger_id == 0)
        {
            input_motion();
        }
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

        if (!output->activate_plugin(&grab_interface))
        {
            return false;
        }

        input_grab->grab_input(wf::scene::layer::OVERLAY, true);
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

        input_grab->ungrab_input();
        output->deactivate_plugin(&grab_interface);

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
            output->emit(&workspace_may_changed);
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
        if (input_grab->is_grabbed())
        {
            input_pressed(WLR_BUTTON_RELEASED);
        }

        output->rem_binding(&activate_binding);
        output->rem_binding(&activate_binding_keep_ratio);
    }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_resize>);
