package dev.hl2craft.client;

import dev.hl2craft.combat.HL2Combat;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.rendering.v1.EntityRendererRegistry;
import net.minecraft.client.renderer.entity.NoopRenderer;

public final class HL2CraftClient implements ClientModInitializer {
	@Override
	public void onInitializeClient() {
		EntityRendererRegistry.register(HL2Combat.HL2_ACTOR, HL2ActorRenderer::new);
		ClientTickEvents.END_CLIENT_TICK.register(mc -> {
			MirrorWorld.openWhenReady(mc);
		});
	}
}
