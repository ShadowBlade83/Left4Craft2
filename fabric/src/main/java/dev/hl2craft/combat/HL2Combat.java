package dev.hl2craft.combat;

import dev.hl2craft.HL2Craft;
import dev.hl2craft.link.HL2Link;
import dev.hl2craft.link.Proto;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.object.builder.v1.entity.FabricDefaultAttributeRegistry;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.MobCategory;
import org.jspecify.annotations.Nullable;

public final class HL2Combat {
	public static final ResourceKey<EntityType<?>> HL2_ACTOR_KEY =
		ResourceKey.create(Registries.ENTITY_TYPE, Identifier.fromNamespaceAndPath(HL2Craft.MOD_ID, "hl2_actor"));

	public static final EntityType<HL2ActorEntity> HL2_ACTOR = Registry.register(
		BuiltInRegistries.ENTITY_TYPE,
		HL2_ACTOR_KEY,
		EntityType.Builder.<HL2ActorEntity>of(HL2ActorEntity::new, MobCategory.MISC)
			.sized(0.6F, 1.8F)
			.noSave()
			.noSummon()
			.noLootTable()
			.clientTrackingRange(10)
			.updateInterval(1)
			.build(HL2_ACTOR_KEY)
	);

	private static final Map<Integer, HL2ActorEntity> PROXIES = new HashMap<>();
	private static final List<HL2Link.Actor> ACTORS = new ArrayList<>();

	private HL2Combat() {
	}

	public static void init() {
		FabricDefaultAttributeRegistry.register(HL2_ACTOR, LivingEntity.createLivingAttributes());
		ServerTickEvents.END_SERVER_TICK.register(HL2Combat::serverTick);
	}

	public static @Nullable HL2ActorEntity proxy(int entIndex) {
		return PROXIES.get(entIndex);
	}

	private static void removeAll() {
		for (HL2ActorEntity proxy : PROXIES.values()) {
			proxy.discard();
		}
		PROXIES.clear();
	}

	private static void serverTick(MinecraftServer server) {
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (!HL2Link.active() || players.isEmpty()) {
			removeAll();
			return;
		}
		ServerLevel level = players.getFirst().level();
		if (HL2Link.readActors(ACTORS)) {
			sync(level);
		}
		for (HL2ActorEntity proxy : PROXIES.values()) {
			float[] hit = proxy.takeHit();
			if (hit != null && (hit[0] > 0.0F || hit[3] > 0.0F)) {
				HL2Link.pushEvent(
					Proto.EV_HIT_ACTOR,
					proxy.entIndex(),
					hit[0],
					hit[1],
					hit[2],
					hit[3],
					Float.floatToRawIntBits(hit[4]),
					Float.floatToRawIntBits(hit[5])
				);
			}
		}
	}

	private static void sync(ServerLevel level) {
		Map<Integer, HL2Link.Actor> live = new HashMap<>();
		for (HL2Link.Actor a : ACTORS) {
			if (!a.dead()) {
				live.put(a.entIndex(), a);
			}
		}
		for (Iterator<Map.Entry<Integer, HL2ActorEntity>> it = PROXIES.entrySet().iterator(); it.hasNext(); ) {
			Map.Entry<Integer, HL2ActorEntity> e = it.next();
			HL2ActorEntity proxy = e.getValue();
			if (!live.containsKey(e.getKey()) || proxy.isRemoved() || proxy.level() != level) {
				proxy.discard();
				it.remove();
			}
		}
		for (HL2Link.Actor a : live.values()) {
			HL2ActorEntity proxy = PROXIES.get(a.entIndex());
			double targetY = a.y();
			double floorY = dev.hl2craft.world.HL2Collision.ensurePlayerFloorAndStep(a.x(), a.y(), a.z(), a.y());
			if (!Double.isNaN(floorY) && Math.abs(a.y() - floorY) < 0.6) {
				targetY = Math.max(a.y(), floorY) + 0.02;
			}
			if (proxy == null) {
				proxy = new HL2ActorEntity(HL2_ACTOR, level);
				proxy.setEntIndex(a.entIndex());
				proxy.setSize(a.width(), a.height());
				proxy.snapTo(a.x(), targetY, a.z(), a.yaw(), 0.0F);
				proxy.setCustomNameVisible(false);
				proxy.setActorInfo(a.flags(), a.name());
				if (!level.addFreshEntity(proxy)) {
					continue;
				}
				PROXIES.put(a.entIndex(), proxy);
				continue;
			}
			proxy.setSize(a.width(), a.height());
			proxy.setPos(a.x(), targetY, a.z());
			proxy.setYRot(a.yaw());
			proxy.setYHeadRot(a.yaw());
			proxy.setActorInfo(a.flags(), a.name());
		}
	}
}
