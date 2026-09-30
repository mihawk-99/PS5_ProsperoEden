        // The HUD is laid out for 1080 lines; a 2160-line frame draws it twice as large.
        const uint32_t hud_scale = render_area.height >= 2 * 1080 ? render_area.height / 1080 : 1;
        if (hud.width && (hud.loading || (render_area.width >= (hud.width + 16) * hud_scale &&
                                          render_area.height >= 64 * hud_scale))) {
            auto text = hud;
            text.x *= hud_scale;
            text.y *= hud_scale;
            text.scale = hud_scale;
            const auto edge = static_cast<int32_t>(16 * hud_scale);
            const VkRect2D scissor = hud.loading ? VkRect2D{{0, 0}, render_area} :
                                                 VkRect2D{{edge, edge}, {hud.width * hud_scale, 48 * hud_scale}};
            const VkViewport viewport{static_cast<float>(scissor.offset.x), static_cast<float>(scissor.offset.y),
                static_cast<float>(scissor.extent.width), static_cast<float>(scissor.extent.height), 0, 1};
            cmdbuf.SetViewport(0, viewport);
            cmdbuf.SetScissor(0, scissor);
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, hud_graphics_pipeline);
            cmdbuf.PushConstants(hud_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, text);
            cmdbuf.Draw(4, 1, 0, 0);
        }
