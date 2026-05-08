! Minimal stub for Flash-X RuntimeParameters_interface required by RungeKutta_init.
! Hardcodes the two parameters used by the RungeKutta module to their
! Config-file defaults (rk_stepSizeConfinementFactor=0.5, rk_stepSizeSafetyFactor=0.9).
module RuntimeParameters_interface
  implicit none
  interface RuntimeParameters_get
    module procedure rp_get_real
  end interface RuntimeParameters_get
contains
  subroutine rp_get_real(name, val)
    character(len=*), intent(in)  :: name
    real,             intent(out) :: val
    select case (trim(name))
    case ("rk_stepSizeConfinementFactor")
      val = 0.5
    case ("rk_stepSizeSafetyFactor")
      val = 0.9
    case default
      write(*, '("RuntimeParameters_get: unknown parameter: ", a)') trim(name)
      error stop
    end select
  end subroutine rp_get_real
end module RuntimeParameters_interface
