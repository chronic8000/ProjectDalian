// Land vehicle weapons (tank/APC) — driver/gunner main gun and coax.

if (state_.player_seat > 1) return;

const VehicleWeaponSlot& main_gun =

    hv.weapons.main_gun.valid ? hv.weapons.main_gun : hv.weapons.coax_gun;

if (!main_gun.valid) return;



state_.heli_gun_cd = std::max(0.f, state_.heli_gun_cd - dt);

if (!input.fire || !input.mouse_look || input.deploy_open || state_.heli_gun_cd > 0.f) return;



const float hd = glm::radians(hv.heading);

const glm::vec3 nose(std::sin(hd), 0.f, std::cos(hd));

const glm::vec3 muzzle = hv.pos + glm::vec3(0.f, 2.2f, 0.f) + nose * 3.5f;

const glm::vec3 dir = input.look_forward;

state_.heli_gun_cd = main_gun.fire_rate;



if (main_gun.projectile.valid || main_gun.role == VehicleWeaponRole::MainCannon) {

  fire_vehicle_projectile(muzzle, dir, main_gun);

  return;

}



const float sp = main_gun.min_deviation;

const glm::vec3 sd =

    glm::normalize(dir + glm::vec3((frand() - 0.5f) * 2.f * sp, (frand() - 0.5f) * 2.f * sp,

                                 (frand() - 0.5f) * 2.f * sp));

const auto eh = shoot_enemies(muzzle, sd, main_gun.range);

if (eh.idx >= 0) {

  damage_enemy(eh.idx, eh.zone, main_gun.damage);

  state_.tracers.push_back({muzzle, eh.point, 0.06f});

  state_.impacts.push_back({eh.point, 0.45f});

} else if (params_.world) {

  const auto hit = params_.world->raycast({muzzle.x, muzzle.y, muzzle.z}, {sd.x, sd.y, sd.z},

                                          main_gun.range);

  const glm::vec3 end = hit.hit ? glm::vec3(hit.point.x, hit.point.y, hit.point.z)

                                : muzzle + sd * main_gun.range;

  state_.tracers.push_back({muzzle, end, 0.06f});

  if (hit.hit) state_.impacts.push_back({end, 0.55f});

}

