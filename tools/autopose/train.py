"""Trains the auto-poser: from the current pose and a few pinned points, the pose a person would take.

Each sample pairs two frames of one clip: the context (what the character is doing now) and the target (where it ends
up). Some of the target's points are given as pins; the network predicts every point. Positions are in the context's
body frame (pelvis origin, X toward the left hip, Y up) and in leg lengths, so any humanoid of any size can use it.

    python -I train.py X:/LilaData/autopose/poses.npz X:/Lila/resources/models/autopose.lilanet
"""

import struct
import sys
import time

import numpy as np
import torch
from torch import nn

POINTS = [
    "pelvis", "spine_02", "spine_04", "neck_01", "head",
    "upperarm_l", "lowerarm_l", "hand_l", "thigh_l", "calf_l", "foot_l", "ball_l",
    "upperarm_r", "lowerarm_r", "hand_r", "thigh_r", "calf_r", "foot_r", "ball_r",
]
P = len(POINTS)
IDX = {n: i for i, n in enumerate(POINTS)}
MIRROR = [IDX[n.replace("_l", "_#").replace("_r", "_l").replace("_#", "_r")] for n in POINTS]
# What an animator grabs most: these are pinned more often in training.
PIN_WEIGHT = torch.tensor([3.0 if n in ("pelvis", "head", "hand_l", "hand_r", "foot_l", "foot_r") else 1.0 for n in POINTS])
CONTEXT_FRAMES = 30  # how far apart context and target may be, frames at 30 fps
HIDDEN = 1024
INPUTS = P * 3 * 2 + P


def body_frame(pts):
    """Origin, rotation (rows = axes) and scale of each pose's body frame. pts: [B, P, 3]."""
    origin = pts[:, IDX["pelvis"]]
    left = pts[:, IDX["thigh_l"]] - pts[:, IDX["thigh_r"]]
    left = torch.stack([left[:, 0], torch.zeros_like(left[:, 0]), left[:, 2]], 1)
    x = left / left.norm(dim=1, keepdim=True).clamp_min(1e-6)
    y = torch.tensor([0.0, 1.0, 0.0], device=pts.device).expand_as(x)
    z = torch.cross(x, y, dim=1)
    rot = torch.stack([x, y, z], 1)
    leg = 0.0
    for s in "lr":
        leg = leg + (pts[:, IDX[f"thigh_{s}"]] - pts[:, IDX[f"calf_{s}"]]).norm(dim=1)
        leg = leg + (pts[:, IDX[f"calf_{s}"]] - pts[:, IDX[f"foot_{s}"]]).norm(dim=1)
    return origin, rot, (leg / 2).clamp_min(1.0)


def to_body(pts, origin, rot, scale):
    return torch.einsum("bij,bpj->bpi", rot, pts - origin[:, None]) / scale[:, None, None]


class Net(nn.Module):
    def __init__(self):
        super().__init__()
        self.layers = nn.Sequential(
            nn.Linear(INPUTS, HIDDEN), nn.SiLU(),
            nn.Linear(HIDDEN, HIDDEN), nn.SiLU(),
            nn.Linear(HIDDEN, HIDDEN), nn.SiLU(),
            nn.Linear(HIDDEN, P * 3),
        )

    def forward(self, current, pins, mask):
        """current, pins: [B, P, 3] body frame; mask: [B, P]. Returns the predicted pose, [B, P, 3]."""
        x = torch.cat([current.flatten(1), (pins * mask[..., None]).flatten(1), mask], 1)
        # A residual on the current pose: with nothing pinned that moved, the answer is to stay put.
        return current + self.layers(x).view(-1, P, 3)


class Sampler:
    def __init__(self, points, clip, device, clips):
        keep = np.isin(clip, clips)
        self.points = torch.tensor(points[keep], device=device)
        clip = clip[keep]
        starts = np.r_[0, np.flatnonzero(np.diff(clip)) + 1]
        ends = np.r_[starts[1:], len(clip)]
        first = np.repeat(starts, ends - starts)
        last = np.repeat(ends, ends - starts) - 1
        self.first = torch.tensor(first, device=device)
        self.last = torch.tensor(last, device=device)
        self.device = device

    def batch(self, n, generator=None):
        d = self.device
        target = torch.randint(0, len(self.points), (n,), device=d, generator=generator)
        offset = torch.randint(-CONTEXT_FRAMES, CONTEXT_FRAMES + 1, (n,), device=d, generator=generator)
        offset = torch.where(torch.rand(n, device=d, generator=generator) < 0.15, torch.zeros_like(offset), offset)
        context = torch.minimum(torch.maximum(target + offset, self.first[target]), self.last[target])
        a, b = self.points[target], self.points[context]
        origin, rot, scale = body_frame(b)
        a, b = to_body(a, origin, rot, scale), to_body(b, origin, rot, scale)
        flip = torch.rand(n, device=d, generator=generator) < 0.5
        sign = torch.tensor([-1.0, 1.0, 1.0], device=d)
        a = torch.where(flip[:, None, None], (a * sign)[:, MIRROR], a)
        b = torch.where(flip[:, None, None], (b * sign)[:, MIRROR], b)

        # Pins: a few of the target's points, the often-grabbed ones more often, and some of what held still.
        count = torch.randint(1, 9, (n, 1), device=d, generator=generator)
        gumbel = -torch.log(-torch.log(torch.rand(n, P, device=d, generator=generator).clamp(1e-9, 1 - 1e-9)))
        keys = torch.log(PIN_WEIGHT.to(d))[None] + gumbel
        rank = keys.argsort(1, descending=True).argsort(1)
        mask = (rank < count).float()
        still = ((a - b).norm(dim=2) < 0.03) & (torch.rand(n, P, device=d, generator=generator) < 0.5)
        mask = torch.maximum(mask, still.float())
        return b, a, mask


def export(net, path):
    """Little-endian: magic, version, layer count, then per layer rows, cols, activation (0 none, 1 SiLU), W, b."""
    linear = [m for m in net.layers if isinstance(m, nn.Linear)]
    with open(path, "wb") as f:
        f.write(b"LAPN")
        f.write(struct.pack("<III", 1, len(linear), P))
        for i, m in enumerate(linear):
            w = m.weight.detach().cpu().numpy().astype("<f4")
            b = m.bias.detach().cpu().numpy().astype("<f4")
            f.write(struct.pack("<III", w.shape[0], w.shape[1], 0 if i == len(linear) - 1 else 1))
            f.write(w.tobytes())
            f.write(b.tobytes())


def main(data, out, steps=60000, batch=4096):
    torch.manual_seed(0)
    d = np.load(data)
    points, clip = d["points"], d["clip"]
    assert list(d["joints"]) == POINTS
    clips = np.unique(clip)
    rng = np.random.default_rng(0)
    held = rng.choice(clips, max(1, len(clips) // 20), replace=False)
    device = "cuda" if torch.cuda.is_available() else "cpu"
    train = Sampler(points, clip, device, np.setdiff1d(clips, held))
    test = Sampler(points, clip, device, held)
    print(f"frames {len(points)}, clips {len(clips)} ({len(held)} held out), device {device}", flush=True)

    net = Net().to(device)
    opt = torch.optim.AdamW(net.parameters(), lr=1e-3, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=1e-3, total_steps=steps, pct_start=0.05)
    fixed = torch.Generator(device=device).manual_seed(1)
    eval_batch = test.batch(65536, fixed)

    def evaluate():
        net.eval()
        with torch.no_grad():
            b, a, mask = eval_batch
            pred = net(b, a, mask)
            free = 1 - mask
            err = ((pred - a).norm(dim=2) * free).sum() / free.sum()
            stay = ((b - a).norm(dim=2) * free).sum() / free.sum()
        net.train()
        # In leg lengths; a leg is about 85 cm.
        return err.item() * 85, stay.item() * 85

    start = time.time()
    for step in range(steps + 1):
        if step % 5000 == 0:
            err, stay = evaluate()
            print(f"step {step:6d}  held-out error {err:5.2f} cm on free points (staying put: {stay:5.2f} cm)  {time.time() - start:6.0f} s", flush=True)
        if step == steps:
            break
        b, a, mask = train.batch(batch)
        loss = (net(b, a, mask) - a).square().sum(2).mean()
        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()
        sched.step()
    export(net, out)
    print("wrote", out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], *(int(v) for v in sys.argv[3:]))
