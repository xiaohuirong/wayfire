#pragma once

#include "wayfire/nonstd/observer_ptr.h"
#include <memory>
#include <vector>
#include <wayfire/region.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/opengl.hpp>

namespace wf
{
class output_t;
namespace scene
{
class node_t;
using node_ptr = std::shared_ptr<node_t>;

class render_instance_t;

/**
 * A single rendering call in a render pass.
 */
struct render_instruction_t
{
    render_instance_t *instance = NULL;
    wf::render_target_t target;
    wf::region_t damage;
};

/**
 * When (parts) of the scenegraph have to be rendered, they have to be
 * 'instantiated' first. The instantiation of a (sub)tree of the scenegraph
 * is a tree of render instances, called a render tree. The purpose of the
 * render trees is to enable damage tracking (each render instance has its own
 * damage), while allowing arbitrary transformations in the scenegraph (e.g. a
 * render instance does not need to export information about how it transforms
 * its children). Due to this design, render trees have to be regenerated every
 * time the relevant portion of the scenegraph changes.
 *
 * Actually painting a render tree (called render pass) is a process involving
 * three steps:
 *
 * 1. Calculate the damage accumulated from the render tree.
 * 2. A front-to-back iteration through the render tree, so that every node
 *   calculates the parts of the destination buffer it should actually repaint.
 * 3. A final back-to-front iteration where the actual rendering happens.
 */
class render_instance_t
{
  public:
    virtual ~render_instance_t() = default;

    /**
     * Handle the front-to-back iteration (2.) from a render pass.
     * Each instance should add the render instructions (calls to
     * render_instance_t::render()) for itself and its children.
     *
     * @param instructions A list of render instructions to be executed.
     *   Instructions are evaluated in the reverse order they are pushed
     *   (e.g. from instructions.rbegin() to instructions.rend()).
     * @param damage The damaged region of the node, in node-local coordinates.
     *   Nodes may subtract from the damage, to prevent rendering below opaque
     *   regions, or expand it for certain special effects like blur.
     * @param fb The target framebuffer to render the node and its children.
     *   Note that some nodes may cause their children to be rendered to
     *   auxilliary buffers.
     */
    virtual void schedule_instructions(
        std::vector<render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::region_t& damage) = 0;

    /**
     * Render the node on the given render target and the given damage region.
     * The node should not paint outside of @region.
     * All coordinates are to be given in the node's parent coordinate system.
     *
     * Note: render() should not be called outside of a render pass.
     *
     * @param target The render target to render the node to, as calculated in
     *   @schedule_instructions.
     * @param region The region to repaint, as calculated in
     *   @schedule_instructions.
     * @param output The output the node is being rendered on. This is just a
     *   hint and may be NULL.
     */
    virtual void render(const wf::render_target_t& target,
        const wf::region_t& region, wf::output_t *output) = 0;
};

using render_instance_uptr = std::unique_ptr<render_instance_t>;

/**
 * A signal emitted when a part of the node is damaged.
 * on: the node itself.
 */
struct node_damage_signal
{
    wf::region_t region;
};

/**
 * Signal that a render pass starts.
 * emitted on: core.
 */
struct render_pass_begin_signal
{
    /**
     * The root instance of the render pass, can be used to identify the render pass.
     */
    nonstd::observer_ptr<render_instance_t> root_instance;

    /**
     * The initial damage for this render pass.
     * Plugins may expand it further.
     */
    wf::region_t damage;

    /**
     * The target buffer for rendering.
     */
    wf::render_target_t target;
};
}
}