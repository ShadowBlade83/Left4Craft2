package dev.hl2craft.mixin;

import dev.hl2craft.combat.HL2ActorEntity;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.target.NearestAttackableTargetGoal;
import net.minecraft.world.entity.animal.golem.IronGolem;
import net.minecraft.world.entity.monster.Enemy;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Mob.class)
public abstract class MobMixin {
	@org.spongepowered.asm.mixin.Shadow
	protected net.minecraft.world.entity.ai.goal.GoalSelector targetSelector;

	@Inject(method = "<init>", at = @At("RETURN"))
	private void hl2craft$targetHL2Actors(EntityType<? extends Mob> type, Level level, CallbackInfo ci) {
		Mob self = (Mob) (Object) this;
		if (this.targetSelector != null) {
			if (self instanceof Enemy) {
				this.targetSelector.addGoal(1, new NearestAttackableTargetGoal<>(self, HL2ActorEntity.class, false));
			} else if (self instanceof IronGolem) {
				this.targetSelector.addGoal(2, new NearestAttackableTargetGoal<>(self, HL2ActorEntity.class, false, (target, sl) -> target instanceof HL2ActorEntity actor && actor.isHostile()));
			}
		}
	}
}
