# Personal network-choice recognizer

This is an experimental finite two-phrase recognizer, not general Chinese ASR.
The two labels are `1 = 有线连接`, `2 = 无线连接`; `0` always means abstain.
No prerecorded speech, credentials, network service, or pretrained model is
included. Only the two algorithm files and synthetic tests belong to this work.

## API and resource bounds

Use heap allocation for `uplink_kw_features` (about 21 KiB) and
`uplink_kw_model` (about 125 KiB). Input is mono signed 16-bit 16 kHz PCM,
0.2–4 seconds (3200–64000 samples); excessive duration and silence fail.
Extraction allocates one bounded workspace of approximately 36 KiB.
DTW allocates two rows, at most 6416 bytes per directional pass, not an
unbounded quadratic matrix. No allocation survives an API call.

Extraction uses 400-sample Hamming windows, 160-sample hop, 512-point FFT,
26 mel filters (80–7600 Hz), relative log energy and 12 cepstral terms.
Peak frame RMS normalization and cepstral mean subtraction reduce gain and
fixed microphone response variation; this does not compensate all rooms or
speakers. A relative -20 dB trim retains 30 ms onset and 80 ms tail.

DTW uses a normalized 0.35 alignment band and a maximum duration ratio of 2.
It weights the first third by 3 because the phrases share their last syllables.
The two directional normalized costs are averaged to preserve symmetry.

Supply three independent training utterances per label with `uplink_kw_add`,
then `uplink_kw_calibrate`. Calibration uses worst leave-one-out same-class
scores and best two-template cross-class scores. It refuses separation no
better than 2.5 times within-class spread. Class radii derive from observed
spread (with a small cross-distance-relative floor), and a classification must
also have a relative winner margin of at least 0.25.

`uplink_kw_evaluate` is for enrollment diagnostics only. Runtime
`uplink_kw_classify` refuses until `uplink_kw_validate` has accepted all three:
one held-out label-1 utterance, one held-out label-2 utterance, and one correctly
rejected unrelated phrase. Exact feature copies of training data cannot be
passed off as held-out validation. A failed validation clears the readiness
mask. Adding/recalibrating templates also clears readiness.

## Required real acceptance tests (not yet performed here)

Independent examples from the intended speaker and hardware are mandatory.
One held-out example per kind is only the API minimum, not enough evidence of
production accuracy. Evaluate multiple fresh utterances at expected distance,
volume and pace, after restarts, plus silence, hum, clicks, unrelated sentences,
questions mentioning the choices, and the confusable opposite phrase. If
templates overlap or held-out results fail, keep boot selection disabled and
collect better data or choose a trained KWS implementation. Do not loosen the
threshold merely to make enrollment pass. Repeated ambiguous speech should
request a retry rather than starting either network mode.

Synthetic tests demonstrate deterministic DSP, finite/bounded handling,
approximate amplitude invariance, symmetry, onset weighting, threshold
calibration, readiness, and ambiguity/out-of-domain feature rejection only.
They do not establish a speech recognition accuracy or false-accept rate.
