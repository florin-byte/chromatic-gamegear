import sys, cv2
d = cv2.QRCodeDetector()
for f in sys.argv[1:]:
    img = cv2.imread(f)
    res = []
    for scale in (1.0, 0.5, 0.35):
        for blur in (0, 3):
            im = cv2.resize(img, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
            if blur: im = cv2.GaussianBlur(im, (blur, blur), 0)
            t, pts, _ = d.detectAndDecode(im)
            res.append("s%.2f/b%d:%s" % (scale, blur, "OK" if t else "--"))
    t, _, _ = d.detectAndDecode(img)
    print(f, " ".join(res), "|", t)
