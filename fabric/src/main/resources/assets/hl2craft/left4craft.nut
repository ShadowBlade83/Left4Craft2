// Left4Craft: the server half inside Left 4 Dead 2 (docs/CONTRACT.md).
// Loaded by the server plugin with `script_execute left4craft`; the plugin calls L4C.* through `script ...`
// and reads what we write to ems/left4craft/out.txt (acknowledged with L4C.Ack, so nothing is lost).
// All positions here are Source units; block keys (bx, by, bz) are Minecraft block coordinates.

if (!("L4C" in getroottable()))
	::L4C <- {};

L4C.version <- 1;
L4C.out <- [];              // unacknowledged lines: [seq, json]
L4C.seq <- 0;
L4C.blocks <- {};           // "bx,by,bz" -> { ent, center, mins, maxs, hp, max }
L4C.mcSlot <- false;        // Minecraft's item is in hand: L4D2's weapon must not fire
L4C.owned <- true;          // Minecraft owns movement
L4C.dying <- false;
L4C.lastFloorAt <- null;
L4C.floorQueue <- [];
L4C.debugDraw <- false;     // the ReShade add-on draws Minecraft's blocks itself
// HL2Craft port: the add-on owns position (setpos), the floor scan and input; this script only keeps the
// barricades (blockers, hit points, breaks) and reports breaks through out.txt.
L4C.portMode <- true;
L4C.UPB <- 40.0;

const L4C_MOVETYPE_NONE = 0;
const L4C_MOVETYPE_WALK = 2;
const L4C_DMG_SLASH = 4;
const L4C_MASK_SHOT = 0x4600400B;
const L4C_MASK_PLAYERSOLID = 0x0201400B;
const L4C_ZOMBIE_TANK = 8;

function L4C::Host()
{
	return GetListenServerHost();
}

function L4C::Emit(json)
{
	L4C.seq++;
	L4C.out.append([L4C.seq, json]);
	if (L4C.out.len() > 200)
		L4C.out.remove(0); // the plugin is gone or stuck: keep the newest
}

// Write every unacknowledged line; the plugin reads the file, forwards the lines and acks the last seq.
function L4C::Flush()
{
	if (L4C.out.len() == 0)
		return;
	local s = "L4C1\n";
	foreach (e in L4C.out)
		s += e[0].tostring() + " " + e[1] + "\n";
	s += "END\n";
	StringToFile("left4craft/out.txt", s);
}

function L4C::Ack(seq)
{
	while (L4C.out.len() > 0 && L4C.out[0][0] <= seq)
		L4C.out.remove(0);
}

function L4C::Fmt(v)
{
	return format("%.2f", v);
}

function L4C::Vec(v)
{
	return "[" + L4C.Fmt(v.x) + "," + L4C.Fmt(v.y) + "," + L4C.Fmt(v.z) + "]";
}

// ---------------------------------------------------------------- movement (Minecraft owns it)

function L4C::SetMoveType(p, t)
{
	NetProps.SetPropInt(p, "movetype", t);
}

// The add-on moves the survivor with setpos while Minecraft owns movement. It stays a walking player (the infected
// neither chase nor hit a noclip survivor, so with noclip they did no damage at all) and only floats: near-zero
// gravity so it doesn't sink between setpos frames. Normal gravity again when L4D2 takes over (ladder, vehicle,
// pinned, incapacitated).
function L4C::SetPuppet(on)
{
	local p = L4C.Host();
	if (p == null || p.IsDead())
		return;
	if (NetProps.GetPropInt(p, "movetype") != L4C_MOVETYPE_WALK)
		L4C.SetMoveType(p, L4C_MOVETYPE_WALK);
	p.SetGravity(on ? 0.0001 : 1.0);
}

// Steve's feet and velocity from Minecraft, every server frame the plugin has a new one.
function L4C::Pose(x, y, z, vx, vy, vz)
{
	local p = L4C.Host();
	if (p == null || !L4C.owned || p.IsDead())
		return;
	if (NetProps.GetPropInt(p, "movetype") != L4C_MOVETYPE_NONE)
		L4C.SetMoveType(p, L4C_MOVETYPE_NONE);
	p.SetOrigin(Vector(x, y, z));
	p.SetVelocity(Vector(vx, vy, vz));
}

// L4D2 takes the survivor when something has hold of it; Minecraft gets it back afterwards.
function L4C::CheckOwnership(p)
{
	local held = p.IsIncapacitated() || p.IsHangingFromLedge() || p.IsDominatedBySpecialInfected() || p.IsDead();
	local owned = !held;
	if (owned != L4C.owned)
	{
		L4C.owned = owned;
		L4C.SetMoveType(p, owned ? L4C_MOVETYPE_NONE : L4C_MOVETYPE_WALK);
		L4C.Emit("{\"t\":\"own\",\"mc\":" + (owned ? "true" : "false") + ",\"o\":" + L4C.Vec(p.GetOrigin()) + "}");
		if (owned)
			L4C.Emit("{\"t\":\"resync\",\"o\":" + L4C.Vec(p.GetOrigin()) + "}");
	}
	else if (!owned)
	{
		L4C.Emit("{\"t\":\"own\",\"mc\":false,\"o\":" + L4C.Vec(p.GetOrigin()) + "}");
	}
}

// ---------------------------------------------------------------- barricades

function L4C::Key(bx, by, bz)
{
	return format("%d,%d,%d", bx, by, bz);
}

// One Minecraft block: an invisible blocker the size of the block, with hit points.
function L4C::Block(bx, by, bz, x0, y0, z0, x1, y1, z1, hp)
{
	local key = L4C.Key(bx, by, bz);
	L4C.Clear(bx, by, bz);
	local center = Vector((x0 + x1) * 0.5, (y0 + y1) * 0.5, (z0 + z1) * 0.5);
	local half = Vector((x1 - x0) * 0.5, (y1 - y0) * 0.5, (z1 - z0) * 0.5);
	local ent = SpawnEntityFromTable("env_physics_blocker", {
		targetname = "l4c_block",
		origin = center,
		mins = Vector(-half.x, -half.y, -half.z),
		maxs = half,
		initialstate = 1,
		BlockType = 0
	});
	L4C.blocks[key] <- { ent = ent, center = center, half = half, hp = hp, max = hp, bx = bx, by = by, bz = bz };
}

function L4C::Clear(bx, by, bz)
{
	local key = L4C.Key(bx, by, bz);
	if (key in L4C.blocks)
	{
		local b = L4C.blocks[key];
		if (b.ent != null && b.ent.IsValid())
			b.ent.Kill();
		delete L4C.blocks[key];
	}
}

// Briefly not solid (the add-on pushes the player out of a block placed on him)
function L4C::Unsolid(bx, by, bz, seconds)
{
	local key = L4C.Key(bx, by, bz);
	if (!(key in L4C.blocks))
		return;
	local e = L4C.blocks[key].ent;
	if (e == null || !e.IsValid())
		return;
	DoEntFire("!self", "Disable", "", 0.0, null, e);
	DoEntFire("!self", "Enable", "", seconds, null, e);
}

function L4C::ClearAll()
{
	foreach (key, b in L4C.blocks)
		if (b.ent != null && b.ent.IsValid())
			b.ent.Kill();
	L4C.blocks <- {};
}

// Infected pressed against a block wear it down: commons a little, a Tank a lot.
function L4C::DamageBlocks(dt)
{
	local broken = [];
	foreach (key, b in L4C.blocks)
	{
		local reach = b.half.x + 34.0;
		local dmg = 0.0;
		local inf = null;
		while ((inf = Entities.FindByClassnameWithin(inf, "infected", b.center, reach)) != null)
		{
			if (inf.GetHealth() > 0)
				dmg += 10.0 * dt;
		}
		local pl = null;
		while ((pl = Entities.FindByClassnameWithin(pl, "player", b.center, reach + 20.0)) != null)
		{
			if (!pl.IsSurvivor() && !pl.IsDead() && pl.GetZombieType() == L4C_ZOMBIE_TANK)
				dmg += 200.0 * dt;
		}
		if (dmg <= 0.0)
			continue;
		b.hp -= dmg;
		if (b.hp <= 0.0)
			broken.append(key);
		else
			L4C.Emit("{\"t\":\"blockdmg\",\"p\":[" + L4C.Key(b.bx, b.by, b.bz) + "],\"hp\":" + b.hp.tointeger().tostring() + ",\"max\":" + b.max.tostring() + "}");
	}
	foreach (key in broken)
	{
		local b = L4C.blocks[key];
		L4C.Emit("{\"t\":\"blockbreak\",\"p\":[" + L4C.Key(b.bx, b.by, b.bz) + "]}");
		L4C.Clear(b.bx, b.by, b.bz);
	}
}

function L4C::DrawBlocks()
{
	if (!L4C.debugDraw)
		return;
	foreach (key, b in L4C.blocks)
	{
		local f = b.hp / b.max;
		DebugDrawBox(b.center, Vector(-b.half.x, -b.half.y, -b.half.z), b.half, (255 * (1.0 - f)).tointeger(), (200 * f).tointeger(), 60, 40, 0.15);
	}
}

// ---------------------------------------------------------------- combat

// A Minecraft sword or axe swing: hit what is under the crosshair within reach, else the nearest infected in front.
function L4C::Melee(dmg, reach)
{
	local p = L4C.Host();
	if (p == null || p.IsDead())
		return;
	local eye = p.EyePosition();
	local fwd = p.EyeAngles().Forward();
	local tr = { start = eye, end = eye + fwd * reach, ignore = p, mask = L4C_MASK_SHOT };
	TraceLine(tr);
	local target = null;
	if (tr.hit && ("enthit" in tr) && tr.enthit != null && tr.enthit.GetClassname() != "worldspawn")
		target = tr.enthit;
	if (target == null)
	{
		local best = 0.85;
		local e = null;
		while ((e = Entities.FindInSphere(e, eye, reach)) != null)
		{
			local cls = e.GetClassname();
			if (cls != "infected" && cls != "witch" && !(cls == "player" && !e.IsSurvivor()))
				continue;
			local to = e.GetOrigin() + Vector(0, 0, 40) - eye;
			local len = to.Norm();
			local d = fwd.Dot(to);
			if (d > best)
			{
				best = d;
				target = e;
			}
		}
	}
	if (target != null)
		target.TakeDamage(dmg, L4C_DMG_SLASH, p);
}

// Minecraft's item is in hand: keep L4D2's weapon from firing or shoving.
function L4C::Slot(n, l4d2)
{
	L4C.mcSlot = !l4d2;
}

function L4C::HoldFire(p)
{
	if (!L4C.mcSlot)
		return;
	local w = p.GetActiveWeapon();
	if (w == null)
		return;
	local later = Time() + 0.3;
	NetProps.SetPropFloat(w, "m_flNextPrimaryAttack", later);
	NetProps.SetPropFloat(w, "m_flNextSecondaryAttack", later);
}

// Minecraft's player died: so does the survivor (L4D2's own death and rescue follow).
function L4C::Die()
{
	local p = L4C.Host();
	if (p == null || p.IsDead())
		return;
	L4C.dying = true;
	p.TakeDamage(10000.0, 0, null);
	if (!p.IsDead())
		p.TakeDamage(10000.0, 0, null);
}

// ---------------------------------------------------------------- the floor for Minecraft

// Down-traces around the survivor: each column's floor (or "wall" when the trace starts inside something).
function L4C::ProbeFloor(p)
{
	local at = p.GetOrigin();
	if (L4C.lastFloorAt == null || (at - L4C.lastFloorAt).Length() > 4.0 * L4C.UPB)
	{
		L4C.lastFloorAt = at;
		L4C.floorQueue = [];
		local r = 16;
		for (local i = -r; i <= r; i++)
			for (local j = -r; j <= r; j++)
				if (i * i + j * j <= r * r)
					L4C.floorQueue.append([i, j]);
		// pop() takes from the end: farthest first in the array, so the nearest columns go out first
		L4C.floorQueue.sort(function(a, b) { return (b[0] * b[0] + b[1] * b[1]) - (a[0] * a[0] + a[1] * a[1]); });
	}
	if (L4C.floorQueue.len() == 0)
		return;
	local centre = L4C.lastFloorAt;
	local s = "";
	local n = 0;
	while (L4C.floorQueue.len() > 0 && n < 120)
	{
		local c = L4C.floorQueue.pop();
		n++;
		local x = centre.x + c[0] * L4C.UPB;
		local y = centre.y + c[1] * L4C.UPB;
		local top = centre.z + 100.0;
		local tr = { start = Vector(x, y, top), end = Vector(x, y, centre.z - 600.0), mask = L4C_MASK_PLAYERSOLID, ignore = p };
		TraceLine(tr);
		local wall = ("startsolid" in tr) && tr.startsolid;
		if (!tr.hit && !wall)
			continue;
		local z = wall ? top : tr.pos.z;
		s += (s == "" ? "" : ",") + L4C.Fmt(x) + "," + L4C.Fmt(y) + "," + L4C.Fmt(z) + "," + (wall ? "1" : "0");
	}
	if (s != "")
		L4C.Emit("{\"t\":\"floor\",\"base\":" + L4C.Fmt(centre.z) + ",\"p\":[" + s + "]}");
}

// ---------------------------------------------------------------- events

L4C.Events <- {};

function L4C::IsHost(userid)
{
	local p = L4C.Host();
	return p != null && p.GetPlayerUserId() == userid;
}

// Minecraft owns health: the survivor keeps 100 and Minecraft takes the hit.
function L4C::Events::OnGameEvent_player_hurt(params)
{
	// HL2Craft port: the add-on reads the survivor's health drop as Minecraft damage and mirrors Minecraft's health
	// back (x5). Topping it up to 100 here (the first bridge's design) hid every hit: the infected did no damage.
	if (L4C.portMode)
	{
		if (L4C.IsHost(params.userid))
			printl("[left4craft] survivor hurt: " + params.dmg_health + " hp, now " + L4C.Host().GetHealth());
		return;
	}
	if (!L4C.IsHost(params.userid) || L4C.dying)
		return;
	local p = L4C.Host();
	local d = params.dmg_health;
	if (d > 0)
		L4C.Emit("{\"t\":\"hurt\",\"d\":" + d.tostring() + "}");
	if (!p.IsIncapacitated() && !p.IsDead())
	{
		p.SetHealth(100);
		p.SetHealthBuffer(0.0);
	}
}

function L4C::Events::OnGameEvent_heal_success(params)
{
	if (L4C.IsHost(params.subject))
		L4C.Emit("{\"t\":\"heal\",\"kind\":\"medkit\"}");
}

function L4C::Events::OnGameEvent_pills_used(params)
{
	if (L4C.IsHost(params.userid))
		L4C.Emit("{\"t\":\"heal\",\"kind\":\"pills\"}");
}

function L4C::Events::OnGameEvent_adrenaline_used(params)
{
	if (L4C.IsHost(params.userid))
		L4C.Emit("{\"t\":\"heal\",\"kind\":\"adrenaline\"}");
}

function L4C::Events::OnGameEvent_player_spawn(params)
{
	if (!L4C.IsHost(params.userid))
		return;
	L4C.dying = false;
	local p = L4C.Host();
	L4C.Emit("{\"t\":\"respawn\",\"o\":" + L4C.Vec(p.GetOrigin()) + "}");
}

// Steve has Minecraft's hand, hotbar and hearts: the survivor carries nothing (no weapon in view, nothing to fire)
// and L4D2's HUD stays hidden. HIDEHUD_ALL (1 << 2) | HIDEHUD_HEALTH (1 << 3) | HIDEHUD_CROSSHAIR (1 << 8).
const L4C_HIDEHUD = 268;

function L4C::StripAndHide(p)
{
	local inv = {};
	GetInvTable(p, inv);
	foreach (slot, w in inv)
		if (w != null && w.IsValid())
			w.Kill();
	if (NetProps.GetPropInt(p, "m_Local.m_iHideHUD") != L4C_HIDEHUD)
		NetProps.SetPropInt(p, "m_Local.m_iHideHUD", L4C_HIDEHUD);
}

// ---------------------------------------------------------------- the loop

L4C.lastThink <- Time();

function L4C::Think()
{
	local now = Time();
	local dt = now - L4C.lastThink;
	L4C.lastThink = now;
	local p = L4C.Host();
	if (p != null && !L4C.portMode)
	{
		if (!L4C.placed)
		{
			L4C.placed = true;
			L4C.Emit("{\"t\":\"resync\",\"o\":" + L4C.Vec(p.GetOrigin()) + "}");
		}
		L4C.CheckOwnership(p);
		L4C.HoldFire(p);
		L4C.ProbeFloor(p);
	}
	if (p != null && !p.IsDead())
		L4C.StripAndHide(p);
	L4C.DamageBlocks(dt);
	L4C.DrawBlocks();
	L4C.Flush();
}

// Start (or restart after a map change): a thinking entity, the event callbacks, a hello for the plugin.
function L4C::Start()
{
	local old = Entities.FindByName(null, "l4c_thinker");
	if (old != null)
		old.Kill();
	local t = SpawnEntityFromTable("info_target", { targetname = "l4c_thinker" });
	t.ValidateScriptScope();
	t.GetScriptScope().Think <- function() { L4C.Think(); return 0.1; };
	AddThinkToEnt(t, "Think");
	__CollectGameEventCallbacks(L4C.Events);
	L4C.out = [];
	L4C.blocks <- {};
	L4C.lastFloorAt = null;
	L4C.owned = true;
	L4C.placed <- false;
	L4C.Emit("{\"t\":\"vscript\",\"v\":" + L4C.version.tostring() + ",\"map\":\"" + Director.GetMapName() + "\"}");
	printl("[left4craft] vscript started on " + Director.GetMapName());
}

L4C.Start();
