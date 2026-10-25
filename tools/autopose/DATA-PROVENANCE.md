# Auto-pose training data provenance

Every dataset the auto-pose model is trained on, with its terms as retrieved. Only the trained weights ship. Raw or converted data never goes into this repository or a Lila distribution.

Local copies live in `X:\LilaData\mocap` (`cmu`, `accad`), with HTML snapshots of each terms page in `X:\LilaData\mocap\terms`.

## CMU Graphics Lab Motion Capture Database

- Source: http://mocap.cs.cmu.edu (bulk download from the FAQ: `allasfamc.zip`)
- Retrieved: 2026-10-25
- Used: ASF/AMC skeletal motion. Finger and thumb channels discarded (not captured). Higher-numbered subjects preferred.

Terms, verbatim from the front page:

> This dataset of motions is free for all uses.

> This data is free for use in research projects. You may include this data in commercially-sold products, but you may not resell this data directly, even in converted form. If you publish results obtained using this data, we would appreciate it if you would send the citation to your published paper to jkh+mocap@cs.cmu.edu, and also would add this text to your acknowledgments section: The data used in this project was obtained from mocap.cs.cmu.edu. The database was created with funding from NSF EIA-0196217.

> Note: The "toe" and "hand" joints in our motions tend to be noisy, and may require some smoothing. The "finger" and "thumb" joints are added to the skeleton for editing convenience - we do not actually capture these joints' motions and any such data should be ignored.

Consequence: shipping weights is fine; shipping a pose library or any converted CMU data is not. Do not use AMASS's copy of CMU (AMASS is non-commercial).

## ACCAD Open Motion Project (The Ohio State University)

- Source: https://accad.ohio-state.edu/research/motion-lab/mocap-system-and-data
- Retrieved: 2026-10-25
- Used: Female 1, Male 1, Male 2 BVH sets and the figure BVH clips. Martial arts C3D sets kept for later solving.

Terms, verbatim from the page footer:

> Open Motion Project by ACCAD/The Ohio State University is licensed under a Creative Commons Attribution 3.0 Unported License.

License: https://creativecommons.org/licenses/by/3.0/legalcode. Attribution is required wherever the weights ship (it is in `THIRD-PARTY-NOTICES.md`).

## Excluded

- Purchased mocap packs (Fab, MoCap Online, etc.): vendor licenses treat ML training as a separate right.
- mocapdata.com paid packs: licensed for "artistic renders or as reference" only, non-transferable.
- mocapdata.com free set: CC BY-SA 2.1 JP. Share-alike may bind the weights. Only usable if those weights ship as a separate CC BY-SA file.
- AMASS, LAFAN1, Bandai Namco: non-commercial.

## Shipped model

`resources/models/autopose.lilanet`, trained 2026-10-26 with `train.py` on the output of `prepare.py` over the two datasets above only: 1,092,392 frames from 2,813 clips (2,514 CMU and 299 ACCAD, both sampled at 30 fps), 140 clips held out for testing.
