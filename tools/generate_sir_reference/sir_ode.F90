! SIR epidemic right-hand side, compatible with Flash-X RungeKutta function interface.
! Parameters match the openswmm odesolve-sir-epidemic benchmark definition:
!   beta=0.3, gamma=0.1, N=1
function sir_ode(t, y)
  implicit none
  real, intent(in) :: t
  real, intent(in) :: y(:)
  real             :: sir_ode(1:size(y))

  real, parameter :: beta  = 0.3
  real, parameter :: gamma = 0.1
  real, parameter :: N     = 1.0

  real :: force
  force        = beta * y(1) * y(2) / N
  sir_ode(1)   = -force
  sir_ode(2)   =  force - gamma * y(2)
  sir_ode(3)   =  gamma * y(2)
end function sir_ode
