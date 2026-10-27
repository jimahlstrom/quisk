import quisk_utils


def test_wheel_step_rounds_half_steps_toward_zero():
    assert quisk_utils.wheel_step(120, 120, 50) == 50
    assert quisk_utils.wheel_step(-120, 120, 50) == -50
    assert quisk_utils.wheel_step(180, 120, 50) == 50
    assert quisk_utils.wheel_step(-180, 120, 50) == -50


def test_wheel_step_preserves_whole_multiple_steps():
    assert quisk_utils.wheel_step(240, 120, 50) == 100
    assert quisk_utils.wheel_step(-240, 120, 50) == -100
    assert quisk_utils.wheel_step(180, 120, 1) == 1
    assert quisk_utils.wheel_step(-180, 120, 1) == -1


def test_wheel_step_handles_zero_delta():
    assert quisk_utils.wheel_step(180, 0, 50) == 0


if __name__ == '__main__':
    test_wheel_step_rounds_half_steps_toward_zero()
    test_wheel_step_preserves_whole_multiple_steps()
    test_wheel_step_handles_zero_delta()
    print('wheel_step ok')