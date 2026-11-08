! Deterministic FT8 waveform used only for integration tests.
! SPDX-License-Identifier: GPL-3.0-only
program make_signal
  use iso_fortran_env, only: int16
  use packjt77
  implicit none
  integer, parameter :: nsamples=180000, nwave=79*1920
  integer :: tones(79), i3, n3, i, k, start, ios
  integer(kind=1) :: bits(77)
  integer(int16) :: samples(nsamples)
  real :: wave(nwave), noise, u, v, freq, dt, snr, amplitude
  complex :: unused(1)
  character(len=512) :: filename, arg
  character(len=37) :: message, sent
  integer, allocatable :: seed(:)
  call get_command_argument(1, filename)
  call get_command_argument(2, message)
  call get_command_argument(3, arg)
  read(arg,*) freq
  call get_command_argument(4, arg)
  read(arg,*) dt
  call get_command_argument(5, arg)
  read(arg,*) snr
  i3=-1
  n3=-1
  call genft8(message,i3,n3,sent,bits,tones)
  if (trim(sent) /= trim(message)) stop 1
  call gen_ft8wave(tones,79,1920,2.0,12000.0,freq,unused,wave,0,nwave)
  call random_seed(size=k)
  allocate(seed(k))
  seed=12345+nint(freq)*13
  call random_seed(put=seed)
  start=nint((0.5+dt)*12000)
  amplitude=sqrt(2.0*2500.0/6000.0)*10.0**(snr/20.0)
  do i=1,nsamples
    call random_number(u)
    call random_number(v)
    noise=sqrt(-2.0*log(max(u,1.0e-10)))*cos(8.0*atan(1.0)*v)
    k=i-start
    if (k>=1 .and. k<=nwave) noise=noise+amplitude*wave(k)
    samples(i)=nint(500.0*noise,kind=int16)
  enddo
  open(newunit=k,file=trim(filename),access='stream',form='unformatted',status='replace',convert='little_endian',iostat=ios)
  if(ios/=0) stop 2
  write(k) samples
  close(k)
end program
