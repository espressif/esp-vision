Session ownership, acquisition and recovery
===========================================

:link_to_translation:`zh_CN:[中文]`

What takeover means in v4
-------------------------

All supported physical interfaces can compete for acquisition, but only the first valid request accepted by the device becomes owner. Later requests cannot force out a valid owner. Default ports, DTR, USB insertion order and debug streams do not establish priority.

``route.bind`` and ``route.auto`` are no longer public operations and return ``UNSUPPORTED_OPERATION``. Both user/debug streams transfer with the sole owner. For host B to take over, A must release, or its link/lease must expire, before B acquires.

.. seqdiag::

   seqdiag {
     edge_length = 280;
     default_fontsize = 12;
     A -> Device [label = "hello: acquire=true, nonce A"];
     A <-- Device [label = "session S; user/debug -> A"];
     B -> Device [label = "hello: discovery only"];
     B <-- Device [label = "identity/capabilities; no owner token"];
     B -> Device [label = "hello: acquire=true, nonce B"];
     B <-- Device [label = "SESSION_BUSY; TCP competitor closed"];
     A -> Device [label = "session.release(S)"];
     A <-- Device [label = "release response"];
     Device -> Device [label = "TX completion; release owner"];
     B -> Device [label = "new link / nonce; acquire"];
     B <-- Device [label = "new session; user/debug -> B"];
   }

**Figure 9: Discovery, failed competition and explicit handoff.** Discovery does not authorize B to send Python/file commands, and A cannot transfer its token to B.

Session state machine
---------------------

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     free [label = "FREE\nno owner"];
     owned [label = "OWNED\nhandshake + lease"];
     releasing [label = "RELEASING\nrelease reply pending"];
     free -> owned [label = "valid acquire"];
     owned -> owned [label = "same link + same nonce"];
     owned -> releasing [label = "release prepare"];
     releasing -> free [label = "reply TX complete"];
     owned -> free [label = "expire / link loss / TX failure"];
   }

**Figure 10: Release preparation and completion preserve the link until its reply is sent.** Immediate reacquisition after receiving release may briefly return ``SESSION_RELEASING``. On the same link, retry the same hello nonce within a bounded total deadline; never retry indefinitely.

Initial discovery and acquisition
---------------------------------

Drain startup residue after opening serial; TCP acquires directly. Metadata and payload in the following block are two parts of one frame, not separate JSON frames:

.. code-block:: json

   {"seq":1,"channel":"user.rpc","type":"req","method":"hello","encoding":"json"}

   {"evMuxVersion":4,"acquire":true,"clientNonce":"0123456789abcdef0123456789abcdef"}

Successful payload example with additional capability fields omitted:

.. code-block:: json

   {
     "ok": true,
     "evMuxVersion": 4,
     "session": {
       "id": "89abcdef0123456789abcdef01234567",
       "owner": "tcp",
       "leaseMs": 15000,
       "keepaliveMs": 5000,
       "vmEpoch": 1
     },
     "vmReady": true,
     "routes": {"user": "tcp", "debug": "tcp"}
   }

Match ``rsp/hello`` by seq, channel and method before accepting the new token. Empty hello/capabilities only discover the device. Discovery replies return to the exact ingress link and do not disclose the valid owner ID to contenders.

``clientNonce`` is a 32-character lowercase hexadecimal identifier for one logical acquisition. Reuse it only when retrying that acquisition on the same link; generate a new value for a new socket, reopened serial port or new acquisition. Same-link, same-nonce idempotent hello can return the same valid session, not transparently replay arbitrary operations.

Handshake completion is gated on acquisition-reply TX completion. Generating a sessionId internally does not authorize the host to submit dependent operations before receiving a valid reply.

Keepalive is not application completion
---------------------------------------

.. code-block:: json

   {"seq":2,"channel":"user.rpc","type":"req","method":"session.keepalive","encoding":"json","sessionId":"89abcdef0123456789abcdef01234567"}

   {}

Default keepalive is 5000 ms and lease 15000 ms; authorized application activity also renews the lease. Schedule according to reported values and keep reading/renewing throughout long scripts, file transfers and preview. Do not wait for an operation to finish before renewing.

Successful keepalive confirms current link/session validity and can report VM generation/readiness. It does not prove script completion, fully published files or applied network configuration.

Four identity/correlation dimensions
------------------------------------

.. list-table:: Non-interchangeable fields
   :header-rows: 1
   :widths: 22 42 36

   * - Dimension
     - Purpose
     - What it cannot imply
   * - link generation
     - Reject work left over from an old physical connection.
     - The same port name or TCP slot need not represent the same connection.
   * - ``sessionId``
     - Authorize the exact link owning the current session.
     - Cannot transfer from discovery replies, another host or an old socket.
   * - ``vmEpoch``
     - Identify generations of VM operations and output.
     - Advancing generation does not imply an idle/executable VM or REPL.
   * - ``seq`` + channel + method
     - Correlate a response within one request context.
     - An event, preview or old response with the same seq cannot complete this RPC.

All non-discovery operations carry sessionId. VM-scoped ``file.write``/``script.write``/``script.run``, camera/object/VFS debugging and ``repl.stdin``/``repl.signal`` also carry current vmEpoch. Transport statistics, keepalive, release and background network controls do not require an idle VM, but still require appropriate owner authorization.

Startup VM readiness and soft reset
-----------------------------------

Current initial ``vmEpoch`` is a random nonzero uint32; older v4 builds may report 0 during startup. Transport controls retain the returned value; VM operations wait for a positive epoch and ``vmReady:true``. Nonzero sequence numbers wrap from 4294967295 to 1. Compare using wrap-aware ordering rather than simple numerical greater-than.

Soft reset may preserve sessionId and networking; epoch advances at reset start. ``event/hello`` is supplemental, not sufficient proof. Confirm the new epoch/readiness through a correlated keepalive, then perform a current-generation REPL barrier requiring an actual new marker.

.. blockdiag::

   blockdiag {
     node_width = 200;
     node_height = 60;
     orientation = portrait;
     old [label = "session S, epoch E\nold request / output"];
     begin [label = "soft reset begins\nepoch becomes E+1"];
     wait [label = "vmReady=false\nold queue admission stopped"];
     ready [label = "vmReady=true\nREPL may still run main.py"];
     proof [label = "interrupt if needed\nnew REPL marker"];
     old -> begin -> wait -> ready -> proof;
   }

**Figure 11: Transport liveness, VM readiness and executable REPL are separate conditions.** Restarted ``boot.py``/``main.py`` may still occupy REPL; readiness alone does not permit immediate VFS/camera VM RPCs.

Hard reset/ROM entry invalidates old tokens, epochs, pending requests and decoder state. Rediscover/acquire after runtime returns. Soft reset clears old VM output parsing state; it must not arbitrarily discard an in-progress physical EV-MUX frame buffer.

Disconnects, FIN/RST and silent disappearance
---------------------------------------------

* TCP FIN/RST or confirmed invalid link: reclaim its owner and invalidate old-generation output; create a new connection and nonce before acquisition.
* Actual USB disconnection invalidates the link. Closing a host serial handle or changing DTR alone may not notify firmware; release explicitly.
* Silent USJ/UART disappearance: wait for lease reclamation. Port type and default/fallback status do not grant early acquisition rights.
* A formerly owned TCP link may retain a 30-second idle window before slot reclamation; this differs from the 15-second lease.

Lease release neither reverses completed external side effects nor guarantees the previous program stopped. After acquisition, B should confirm VM state, interrupt explicitly when necessary, and verify executable REPL with a new marker.

Errors and recovery decisions
-----------------------------

.. list-table:: Common errors
   :header-rows: 1
   :widths: 30 70

   * - Error
     - Host response
   * - ``SESSION_BUSY``
     - Another valid owner exists; request release/wait rather than forcing takeover or reusing its token.
   * - ``SESSION_RELEASING``
     - Same-link release completion is pending; retry the same nonce only within bounded acquisition.
   * - ``SESSION_REQUIRED`` / ``NOT_OWNER``
     - Stop old-session operations, discard local session state and establish a new session.
   * - ``VM_EPOCH_MISMATCH``
     - The original VM request is invalid; reconfirm generation and do not automatically replay scripts/file writes.
   * - ``VM_BUSY`` / ``VM_TIMEOUT``
     - VM work cannot queue or was not serviced in time; resolve running scripts, cancellation/reset and actual REPL liveness.
   * - Link TX failure / host timeout
     - Side effects may already exist. Mark completion unknown and query/verify file or transaction state first.

Network transactions with the same ``operationId`` have dedicated correlation/recovery semantics. ``seq`` is not a universal idempotency key. Scripts, REPL input and file append have no general transparent-replay contract.
