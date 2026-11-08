! Standalone integration wrapper, 2026. SPDX-License-Identifier: GPL-3.0-only
module ft8_bridge
  use iso_c_binding
  use ft8_decode, only: ft8_decoder
  implicit none
  private
  public :: ft8_decode_slot

  abstract interface
    subroutine result_callback(message, length, snr, dt, freq, sync, quality, ap, context) bind(C)
      import c_char, c_int, c_float, c_ptr
      character(c_char), intent(in) :: message(*)
      integer(c_int), value :: length, snr, ap
      real(c_float), value :: dt, freq, sync, quality
      type(c_ptr), value :: context
    end subroutine
  end interface
  procedure(result_callback), pointer :: sink => null()
  type(c_ptr) :: sink_context
contains
  ! WSJT-X has process-global saved state. The CLI invokes this once per
  ! process; parallel callers must use separate processes.
  subroutine ft8_decode_slot(samples, low, high, depth, callback, context) bind(C)
    integer(c_int16_t), intent(in) :: samples(180000)
    integer(c_int), value :: low, high, depth
    type(c_funptr), value :: callback
    type(c_ptr), value :: context
    type(ft8_decoder) :: decoder
    integer(c_int16_t) :: partial(180000)
    integer :: phase, nzhsym, nfa, nfb, npatience, nthreads
    logical :: newdat, try_a8
    logical(kind=1) :: diskdata
    character(len=12) :: mycall, hiscall
    character(len=6) :: hisgrid
    common /patience/ npatience, nthreads

    call c_f_procpointer(callback, sink)
    sink_context = context
    npatience = 0
    nthreads = 1
    diskdata = .true.  ! Offline decoding must not depend on the wall clock.
    mycall = ''
    hiscall = ''
    hisgrid = ''
    do phase = 1, 3
      ! Reproduce WSJT-X's receive stages at 11.808, 13.536, and 14.4 s.
      if (phase == 1) nzhsym = 41
      if (phase == 2) nzhsym = 47
      if (phase == 3) nzhsym = 50
      partial = 0
      partial(1:min(180000,nzhsym*3456)) = samples(1:min(180000,nzhsym*3456))
      ! Final pass receives the entire slot, including late-arriving signals.
      if (phase == 3) partial = samples
      newdat = .true.
      try_a8 = .false.
      nfa = low
      nfb = high
      call decoder%decode(receive,partial,0,1500,1500,newdat,0,nfa,nfb, &
        nzhsym,depth,0.0,0,.false.,.false.,try_a8,.false.,50, &
        mycall,hiscall,hisgrid,diskdata)
    enddo
    nullify(sink)
  end subroutine

  subroutine receive(this, sync, snr, dt, freq, decoded, nap, qual)
    class(ft8_decoder), intent(inout) :: this
    real, intent(in) :: sync, dt, freq, qual
    integer, intent(in) :: snr, nap
    character(len=37), intent(in) :: decoded
    character(c_char) :: message(37)
    integer :: i, n
    n = len_trim(decoded)
    do i = 1, n
      message(i) = decoded(i:i)
    enddo
    call sink(message,int(n,c_int),int(snr,c_int),dt,freq,sync,qual,int(nap,c_int),sink_context)
  end subroutine
end module
