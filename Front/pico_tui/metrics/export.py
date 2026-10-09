"""Exportação das métricas e dos ensaios para análise fora da TUI.

Gera, em ``exports/``:
  metricas_<data>.json     retrato completo (barramento, identificadores,
                           enlaces, coordenação, análise de tempo de resposta
                           e ensaios realizados)
  metricas_<data>_ids.csv  uma linha por identificador CAN
  metricas_<data>_bus.csv  série de ocupação do barramento (uma linha por segundo)
"""
from __future__ import annotations

import csv
import json
from collections.abc import Iterable
from datetime import datetime
from pathlib import Path

from pico_tui.metrics.experiments import MEASURE_LABELS, ExperimentRun
from pico_tui.metrics.service import MetricsSnapshot


def _number(value: float | None, decimals: int = 3) -> float | None:
    return None if value is None else round(value, decimals)


def run_to_dict(run: ExperimentRun) -> dict[str, object]:
    metrics: dict[str, object] = {}
    for key, value in run.metrics.items():
        if isinstance(value, (set, frozenset)):
            metrics[key] = sorted(value)
        elif isinstance(value, float):
            metrics[key] = round(value, 3)
        else:
            metrics[key] = value
    return {
        "experiment": run.experiment.key,
        "title": run.title,
        "kind": run.experiment.kind,
        "node": run.node_id,
        "seconds": run.seconds,
        "load_percent": run.load_percent,
        "command": run.command,
        "origin": run.origin,
        "state": run.state,
        "started": datetime.fromtimestamp(run.started_wall).isoformat(timespec="seconds"),
        "observed_seconds": _number(run.finished_after_s),
        "leader_before": run.leader_before,
        "metrics": metrics,
        "labels": {key: MEASURE_LABELS[key][0] for key in run.experiment.measures},
        "events": [{"t": round(event.t, 3), "text": event.text} for event in run.events],
    }


def snapshot_to_dict(snapshot: MetricsSnapshot, runs: Iterable[ExperimentRun] = ()) -> dict[str, object]:
    bus = snapshot.bus
    analysis = snapshot.analysis
    capacity = snapshot.capacity
    coordination = snapshot.coordination
    return {
        "generated": datetime.now().isoformat(timespec="seconds"),
        "observed_seconds": _number(snapshot.now - snapshot.started, 1),
        "bitrate": snapshot.bitrate,
        "bus": {
            "load_percent": bus.load_percent if bus else None,
            "peak_percent": bus.peak_percent if bus else None,
            "max_load_percent": snapshot.max_load_percent,
            "max_peak_percent": snapshot.max_peak_percent,
            "frames_per_second": bus.frames * 1000.0 / bus.window_ms if bus and bus.window_ms else None,
            "rx_errors": bus.rx_errors if bus else None,
            "tx_errors": bus.tx_errors if bus else None,
            "max_rx_errors": snapshot.max_rx_errors,
            "max_tx_errors": snapshot.max_tx_errors,
            "error_state": snapshot.error_state,
            "rx_queue_peak": bus.rx_queue_peak if bus else None,
            "transfers_ok": bus.transfers_ok if bus else None,
            "transfers_err": bus.transfers_err if bus else None,
            "total_frames": snapshot.total_frames,
            "total_bits": snapshot.total_bits,
        },
        "ids": [
            {
                "id": f"0x{item.can_id:03X}",
                "name": item.name,
                "group": item.group,
                "window_ms": item.window_ms,
                "frames": item.count,
                "dlc": item.dlc,
                "bits": item.bits,
                "dt_min_us": item.dt_min_us,
                "dt_avg_us": item.dt_avg_us,
                "dt_max_us": item.dt_max_us,
                "total_frames": item.total_frames,
            }
            for item in snapshot.ids
        ],
        "response_time_analysis": None
        if analysis is None
        else {
            "method": "Davis, Burns, Bril e Lukkien (2007), parâmetros observados",
            "utilization_percent": round(100.0 * analysis.utilization, 3),
            "schedulable": analysis.schedulable,
            "flows": [
                {
                    "id": f"0x{result.flow.can_id:03X}",
                    "name": result.flow.name,
                    "dlc": result.flow.dlc,
                    "period_ms": round(result.flow.period_us / 1000.0, 3),
                    "burst": result.flow.burst,
                    "burst_estimated": result.flow.estimated,
                    "frame_us": round(result.frame_us, 1),
                    "blocking_us": round(result.blocking_us, 1),
                    "response_first_ms": _number(result.response_first_us / 1000.0 if result.response_first_us is not None else None),
                    "response_ms": _number(result.response_us / 1000.0 if result.response_us is not None else None),
                    "deadline_ms": round(result.deadline_us / 1000.0, 3),
                    "slack_percent": _number(result.slack_percent, 2),
                    "schedulable": result.schedulable,
                }
                for result in analysis.results
            ],
            "capacity": None
            if capacity is None
            else {
                "extra_sensors": capacity.extra_sensors,
                "limit_reached": capacity.limit_reached,
                "utilization_percent": round(100.0 * capacity.utilization, 3),
            },
        },
        "links": [
            {
                "sensor": link.logical_id,
                "uuid": link.wireless_uuid,
                "state": link.link_state,
                "rssi_dbm": link.rssi_dbm,
                "rtt_ms": link.rtt_ms,
                "rtt_max_ms": link.rtt_max_ms,
                "sessions": link.sessions,
                "last_recovery_s": _number(link.last_recovery_s),
                "telemetry_count": link.telemetry_count,
                "telemetry_lost": link.telemetry_lost,
                "interval_avg_s": _number(link.interval_avg_s),
                "interval_min_s": _number(link.interval_min_s),
                "interval_max_s": _number(link.interval_max_s),
                "auth_failures": link.auth_failures,
                "replay_drops": link.replay_drops,
            }
            for link in snapshot.links
        ],
        "coordination": {
            "leader": coordination.leader,
            "heartbeat_period_ms": coordination.heartbeat_period_ms,
            "heartbeats": coordination.heartbeats,
            "max_heartbeat_gap_s": _number(coordination.max_heartbeat_gap_s),
            "leader_changes": coordination.leader_changes,
            "elections": coordination.elections,
            "leader_failures": coordination.leader_failures,
            "joins": coordination.joins,
            "failovers": coordination.failovers,
            "nodes_in_fault": sorted(coordination.nodes_in_fault),
        },
        "commands": {
            "count": snapshot.commands.count,
            "rtt_min_ms": snapshot.commands.minimum,
            "rtt_avg_ms": _number(snapshot.commands.average),
            "rtt_p95_ms": snapshot.commands.p95,
            "rtt_max_ms": snapshot.commands.maximum,
            "rejected": snapshot.commands.rejected,
        },
        "experiments": [run_to_dict(run) for run in runs],
    }


def export_metrics(snapshot: MetricsSnapshot, runs: Iterable[ExperimentRun] = (), directory: Path | None = None) -> list[Path]:
    directory = directory or Path.cwd() / "exports"
    directory.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    payload = snapshot_to_dict(snapshot, runs)

    json_path = directory / f"metricas_{stamp}.json"
    json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")

    ids_path = directory / f"metricas_{stamp}_ids.csv"
    results = {result.flow.can_id: result for result in (snapshot.analysis.results if snapshot.analysis else ())}
    with ids_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            ["id", "mensagem", "grupo", "janela_ms", "quadros", "dlc", "bits", "dt_min_us", "dt_avg_us", "dt_max_us",
             "periodo_ms", "rajada", "rajada_estimada", "resposta_ms", "prazo_ms", "folga_pct"]
        )
        for item in snapshot.ids:
            result = results.get(item.can_id)
            writer.writerow(
                [
                    f"0x{item.can_id:03X}", item.name, item.group, item.window_ms, item.count, item.dlc, item.bits,
                    item.dt_min_us, item.dt_avg_us, item.dt_max_us,
                    f"{result.flow.period_us / 1000.0:.3f}" if result else "",
                    result.flow.burst if result else "",
                    ("sim" if result.flow.estimated else "nao") if result else "",
                    f"{result.response_us / 1000.0:.3f}" if result and result.response_us is not None else "",
                    f"{result.deadline_us / 1000.0:.3f}" if result else "",
                    f"{result.slack_percent:.2f}" if result and result.slack_percent is not None else "",
                ]
            )

    bus_path = directory / f"metricas_{stamp}_bus.csv"
    with bus_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["t_s", "janela_ms", "quadros", "bits", "ocupacao_pct", "pico_pct", "rx_err", "tx_err",
                         "fila_rx_pico", "ids", "quadros_probe"])
        for sample in snapshot.bus_history:
            writer.writerow(
                [f"{sample.t - snapshot.started:.3f}", sample.window_ms, sample.frames, sample.bits,
                 f"{sample.load_percent:.2f}", f"{sample.peak_percent:.2f}", sample.rx_errors, sample.tx_errors,
                 sample.rx_queue_peak, sample.ids, sample.own_frames]
            )
    return [json_path, ids_path, bus_path]
