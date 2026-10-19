package dev.hl2craft.client.mixin;

import com.mojang.blaze3d.pipeline.RenderTarget;
import dev.hl2craft.client.FrameExporter;
import dev.hl2craft.client.HL2Client;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyArg;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GameRenderer.class)
abstract class GameRendererMixin {
	@Shadow @Final private RenderTarget mainRenderTarget;

	@ModifyArg(
		method = "renderLevel",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/LevelRenderer;render(Lcom/mojang/blaze3d/resource/GraphicsResourceAllocator;ZLnet/minecraft/client/renderer/state/level/CameraRenderState;Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Lorg/joml/Vector4f;ZZ)V"),
		index = 5
	)
	private boolean hl2craft$noSky(final boolean shouldRenderSky) {
		return shouldRenderSky && !HL2Client.linked();
	}

	@Inject(
		method = "renderLevel",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render3dHud(Lnet/minecraft/client/renderer/state/level/CameraRenderState;Lnet/minecraft/client/renderer/state/level/PlayerRenderState;Lnet/minecraft/client/renderer/state/OptionsRenderState;Z)V")
	)
	private void hl2craft$captureWorld(final CallbackInfo ci) {
		FrameExporter.captureWorld(this.mainRenderTarget);
	}

	@Inject(method = "render", at = @At("TAIL"))
	private void hl2craft$captureOverlay(final CallbackInfo ci) {
		FrameExporter.captureOverlay(this.mainRenderTarget);
	}
}
