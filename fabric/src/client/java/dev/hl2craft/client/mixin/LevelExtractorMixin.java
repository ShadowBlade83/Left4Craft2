package dev.hl2craft.client.mixin;

import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.extract.LevelExtractor;
import net.minecraft.client.renderer.state.level.LevelRenderState;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.Blocks;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(LevelExtractor.class)
public abstract class LevelExtractorMixin {
	@Shadow @Final private Minecraft minecraft;
	@Shadow @Final private LevelRenderState levelRenderState;

	@Inject(method = "extract", at = @At("TAIL"))
	private void hl2craft$clearBarrierOutline(DeltaTracker deltaTracker, Camera camera, float f, CallbackInfo ci) {
		if (this.levelRenderState.blockOutlineRenderState != null) {
			BlockPos pos = this.levelRenderState.blockOutlineRenderState.pos();
			if (this.minecraft.level != null && this.minecraft.level.getBlockState(pos).is(Blocks.BARRIER)) {
				this.levelRenderState.blockOutlineRenderState = null;
			}
		}
	}

	@Inject(method = "setSectionDirty(IIIZ)V", at = @At("HEAD"))
	private void hl2craft$sectionDirty(int sectionX, int sectionY, int sectionZ, boolean playerChanged, CallbackInfo ci) {
		if (playerChanged) {
			dev.hl2craft.client.render.WorldExporter.markDirtyNow(sectionX, sectionY, sectionZ);
		} else {
			dev.hl2craft.client.render.WorldExporter.markDirty(sectionX, sectionY, sectionZ);
		}
	}
}
