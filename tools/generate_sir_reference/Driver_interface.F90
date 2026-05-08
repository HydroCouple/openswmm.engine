! Minimal stub for Flash-X Driver_interface required by RungeKutta module.
module Driver_interface
  implicit none
  interface
    subroutine Driver_abort(errorMessage)
      character(len=*), intent(in) :: errorMessage
    end subroutine Driver_abort
  end interface
end module Driver_interface

subroutine Driver_abort(errorMessage)
  implicit none
  character(len=*), intent(in) :: errorMessage
  write(*, '("ABORT: ", a)') trim(errorMessage)
  error stop
end subroutine Driver_abort
