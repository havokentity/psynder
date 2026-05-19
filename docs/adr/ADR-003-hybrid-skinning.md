# ADR-003: Hybrid skinning — chassis skinned, wheels rigid

- **Status:** Accepted
- **Date:** 2026-05-19

## Context

Vehicles need:

- A chassis that flexes (body roll under load, damage poses, hood
  crumple, panel deformation) — best done with skinning
- Wheels that spin and steer independently of the chassis — driven by
  the physics solver (each wheel is a constrained rigid body in
  `psynder_phys::vehicle`)
- Detachable parts (door blown off, bumper torn off) that become
  standalone physics bodies — flying off with their own rigid mesh

Pure-skinned-everything makes wheels awkward (need a separate
"steering" bone whose transform is driven by physics, then the spinning
sub-bones whose transform is also driven by physics — too many indirect
animation hops). Pure-rigid-everything loses the chassis flex.

The same applies to scripted aircraft (helicopters, planes): the
fuselage flexes; rotor blades + landing gear + control surfaces are
rigid meshes parented to skeleton bones.

## Decision

**Hybrid skinning:**

- **Chassis / fuselage:** skinned to a per-vehicle skeleton. The root
  bone is driven by the physics rigid body; additional bones handle
  cosmetic deformation (damage poses, hood crumple, panel flex, body
  roll on suspension).
- **Wheels, hubcaps, spoilers, mirrors, antennas, rotor blades, control
  surfaces (cosmetic only — aircraft are kinematic, not flight-simmed
  in v1):** rendered as rigid meshes parented to skeleton bones. Vehicle
  wheels read their transforms directly from the physics solver (each
  wheel is a constrained rigid body in `psynder_phys`, one-to-one with
  the rendered mesh). Aircraft surfaces are driven by script /
  animation.
- **Severe damage:** a rigid sub-mesh (door, bumper) detaches and
  becomes its own standalone physics rigid body, flying off with its
  own rigid mesh.
- **Drivers / characters:** standard fully-skinned pipeline with their
  own skeleton.
- LBS (linear blend skinning) is the default; DQS (dual-quaternion)
  available per-material where candy-wrapper artifacts show.

## Consequences

- The vertex pipeline distinguishes skinned and rigid streams; the
  binner sees them identically.
- Wheels-as-physics-bodies means the artist authors a wheel as a separate
  mesh + a `Wheel` component pointing at its physics body. No magic.
- Damage detachment is a runtime operation: lane 13 (`psynder_phys`)
  spawns a new rigid body when a part's "health" hits zero; lane 06
  (ECS) reparents the rendered mesh.

## References

- DESIGN.md §7.2 (vertex pipeline — hybrid skinning paragraph)
- DESIGN.md §10.1 (vehicle module: raycast / sphere-cast suspension,
  Pacejka tires, drivetrain)
