use std::time::Duration;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RateProfile {
    pub id: u8,
    pub inter_byte_sleep: Duration,
}

pub const RATE_PROFILES: [RateProfile; 10] = [
    RateProfile {
        id: 0,
        inter_byte_sleep: Duration::from_micros(10000),
    },
    RateProfile {
        id: 1,
        inter_byte_sleep: Duration::from_micros(7000),
    },
    RateProfile {
        id: 2,
        inter_byte_sleep: Duration::from_micros(5000),
    },
    RateProfile {
        id: 3,
        inter_byte_sleep: Duration::from_micros(3000),
    },
    RateProfile {
        id: 4,
        inter_byte_sleep: Duration::from_micros(2000),
    },
    RateProfile {
        id: 5,
        inter_byte_sleep: Duration::from_micros(1500),
    },
    RateProfile {
        id: 6,
        inter_byte_sleep: Duration::from_micros(1250),
    },
    RateProfile {
        id: 7,
        inter_byte_sleep: Duration::from_micros(1000),
    },
    RateProfile {
        id: 8,
        inter_byte_sleep: Duration::from_micros(750),
    },
    RateProfile {
        id: 9,
        inter_byte_sleep: Duration::from_micros(500),
    },
];

pub struct RateController {
    highest_stable: u8,
    active: u8,
    supported_mask: u16,
    consecutive_losses: u8,
    downshift_count: u8,
}

impl RateController {
    pub fn new() -> Self {
        Self {
            highest_stable: 0,
            active: 0,
            supported_mask: all_profile_mask(),
            consecutive_losses: 0,
            downshift_count: 0,
        }
    }

    pub fn mark_profile_stable(&mut self, id: u8) {
        self.mark_profile_stable_with_mask(id, all_profile_mask());
    }

    pub fn mark_profile_stable_with_mask(&mut self, id: u8, supported_mask: u16) {
        if !profile_exists(id) {
            return;
        }
        self.supported_mask = normalize_supported_mask(supported_mask);
        self.highest_stable = id;
        self.active = highest_supported_below(id, self.supported_mask);
        self.consecutive_losses = 0;
        self.downshift_count = 0;
    }

    pub fn record_loss(&mut self) {
        self.consecutive_losses = self.consecutive_losses.saturating_add(1);
        if self.consecutive_losses < 3 {
            return;
        }

        self.consecutive_losses = 0;
        if self.active > 0 {
            self.active = highest_supported_below(self.active, self.supported_mask);
            self.downshift_count = self.downshift_count.saturating_add(1);
        }
    }

    pub fn record_success(&mut self) {
        self.consecutive_losses = 0;
    }

    pub fn needs_recalibration(&self) -> bool {
        self.downshift_count >= 2
    }

    pub fn active_profile(&self) -> RateProfile {
        RATE_PROFILES
            .iter()
            .copied()
            .find(|profile| profile.id == self.active)
            .unwrap_or(RATE_PROFILES[0])
    }
}

impl Default for RateController {
    fn default() -> Self {
        Self::new()
    }
}

fn profile_exists(id: u8) -> bool {
    RATE_PROFILES.iter().any(|profile| profile.id == id)
}

fn all_profile_mask() -> u16 {
    RATE_PROFILES
        .iter()
        .fold(0u16, |mask, profile| mask | (1u16 << profile.id))
}

fn normalize_supported_mask(supported_mask: u16) -> u16 {
    let known_mask = all_profile_mask();
    (supported_mask & known_mask) | 1u16
}

fn highest_supported_below(stable_id: u8, supported_mask: u16) -> u8 {
    RATE_PROFILES
        .iter()
        .rev()
        .find(|profile| profile.id < stable_id && supported_mask & (1u16 << profile.id) != 0)
        .map(|profile| profile.id)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn rate_profiles_match_v2_plan() {
        assert_eq!(
            RATE_PROFILES,
            [
                RateProfile {
                    id: 0,
                    inter_byte_sleep: Duration::from_micros(10000),
                },
                RateProfile {
                    id: 1,
                    inter_byte_sleep: Duration::from_micros(7000),
                },
                RateProfile {
                    id: 2,
                    inter_byte_sleep: Duration::from_micros(5000),
                },
                RateProfile {
                    id: 3,
                    inter_byte_sleep: Duration::from_micros(3000),
                },
                RateProfile {
                    id: 4,
                    inter_byte_sleep: Duration::from_micros(2000),
                },
                RateProfile {
                    id: 5,
                    inter_byte_sleep: Duration::from_micros(1500),
                },
                RateProfile {
                    id: 6,
                    inter_byte_sleep: Duration::from_micros(1250),
                },
                RateProfile {
                    id: 7,
                    inter_byte_sleep: Duration::from_micros(1000),
                },
                RateProfile {
                    id: 8,
                    inter_byte_sleep: Duration::from_micros(750),
                },
                RateProfile {
                    id: 9,
                    inter_byte_sleep: Duration::from_micros(500),
                },
            ]
        );
    }

    #[test]
    fn active_profile_is_one_below_highest_stable() {
        let mut controller = RateController::new();
        controller.mark_profile_stable(3);
        assert_eq!(controller.active_profile().id, 2);
    }

    #[test]
    fn repeated_losses_downshift_and_request_recalibration() {
        let mut controller = RateController::new();
        controller.mark_profile_stable(3);
        controller.record_loss();
        controller.record_loss();
        controller.record_loss();
        assert_eq!(controller.active_profile().id, 1);
        controller.record_loss();
        controller.record_loss();
        controller.record_loss();
        assert!(controller.needs_recalibration());
    }

    #[test]
    fn success_resets_consecutive_losses() {
        let mut controller = RateController::new();
        controller.mark_profile_stable(3);
        controller.record_loss();
        controller.record_loss();

        controller.record_success();
        controller.record_loss();

        assert_eq!(controller.active_profile().id, 2);
    }

    #[test]
    fn active_profile_uses_supported_profile_below_stable() {
        let mut controller = RateController::new();

        controller.mark_profile_stable_with_mask(2, 0x0005);

        assert_eq!(controller.active_profile().id, 0);
    }

    #[test]
    fn losses_at_p0_do_not_underflow_or_request_recalibration() {
        let mut controller = RateController::new();

        for _ in 0..9 {
            controller.record_loss();
        }

        assert_eq!(controller.active_profile().id, 0);
        assert!(!controller.needs_recalibration());
    }

    #[test]
    fn unsupported_stable_profile_ids_are_ignored() {
        let mut controller = RateController::new();
        controller.mark_profile_stable(2);
        controller.mark_profile_stable(99);

        assert_eq!(controller.active_profile().id, 1);
    }
}
