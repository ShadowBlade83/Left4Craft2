package dev.hl2craft.client;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.hl2craft.combat.HL2ActorEntity;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.entity.EntityRenderer;
import net.minecraft.client.renderer.entity.EntityRendererProvider;
import net.minecraft.client.renderer.entity.state.EntityRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;

public class HL2ActorRenderer extends EntityRenderer<HL2ActorEntity, EntityRenderState> {
	public HL2ActorRenderer(EntityRendererProvider.Context context) {
		super(context);
	}

	@Override
	public void submit(EntityRenderState state, PoseStack poseStack, SubmitNodeCollector collector, CameraRenderState cameraState) {
		// Do not submit any model mesh - the NPC is rendered by Half-Life 2
	}

	@Override
	protected void submitNameDisplay(EntityRenderState state, PoseStack poseStack, SubmitNodeCollector collector, CameraRenderState cameraState) {
		// Suppress nametags completely
	}

	@Override
	public EntityRenderState createRenderState() {
		return new EntityRenderState();
	}
}
