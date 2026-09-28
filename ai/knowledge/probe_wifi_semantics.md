# Probe and Wi-Fi evidence semantics

## Active probes

Gateway and remote probes are namespace- and freshness-scoped observations. A reachable gateway and a degraded remote probe support different attribution than a failed gateway probe. Probe unavailability is a capability limitation and should prompt a diagnostic procedure.

## Wi-Fi evidence

Very weak Wi-Fi signal is evidence about link conditions. It does not confirm RF interference. Association state and signal observations have separate meanings. Missing RF or access-point health evidence should retrieve checks for those observations.

## Diagnostic next steps

When evidence is missing or contradictory, retrieve procedures that explain which fresh probe, association, signal, RF, or AP-health observation would reduce uncertainty. Do not treat a missing observation as supporting or contradicting evidence.
