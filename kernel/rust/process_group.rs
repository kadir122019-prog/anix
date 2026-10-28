#![cfg_attr(not(test), no_std)]

#[cfg(not(test))]
use core::panic::PanicInfo;

#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {
        unsafe { core::arch::asm!("cli; hlt", options(nomem, nostack)); }
    }
}

/// Validate a process-group change requested by a process's parent or by the
/// process itself. The C process table supplies whether the target is in scope
/// and whether the requested group has a live member in that scope.
#[no_mangle]
pub extern "C" fn anix_process_group_change_allowed(
    caller_pid: u64,
    target_pid: u64,
    target_pgid: u64,
    requested_pgid: u64,
    target_is_caller_or_child: u8,
    target_exited: u8,
    group_exists_in_scope: u8,
) -> u8 {
    if caller_pid == 0
        || target_pid == 0
        || requested_pgid == 0
        || target_is_caller_or_child == 0
        || target_exited != 0
    {
        return 0;
    }

    // A target may start a group named by its PID, remain in its current group,
    // or join a group already represented by the caller or one of its children.
    u8::from(
        requested_pgid == target_pid
            || requested_pgid == target_pgid
            || group_exists_in_scope != 0,
    )
}

#[cfg(test)]
mod tests {
    use super::anix_process_group_change_allowed as allowed;

    #[test]
    fn creates_group_for_self_or_child() {
        assert_eq!(allowed(7, 7, 7, 7, 1, 0, 0), 1);
        assert_eq!(allowed(7, 8, 7, 8, 1, 0, 0), 1);
    }

    #[test]
    fn joins_existing_scoped_group() {
        assert_eq!(allowed(7, 8, 8, 7, 1, 0, 1), 1);
    }

    #[test]
    fn rejects_invalid_or_out_of_scope_changes() {
        assert_eq!(allowed(0, 8, 7, 8, 1, 0, 0), 0);
        assert_eq!(allowed(7, 0, 7, 7, 1, 0, 0), 0);
        assert_eq!(allowed(7, 8, 7, 9, 1, 0, 0), 0);
        assert_eq!(allowed(7, 8, 7, 8, 0, 0, 1), 0);
        assert_eq!(allowed(7, 8, 7, 8, 1, 1, 0), 0);
    }
}
