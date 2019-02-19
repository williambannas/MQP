from flask import Flask, jsonify, render_template, request
import numpy as np
import json

app = Flask(__name__)

dataPoints = []
w,h = 7,20;
Signals = [[0 for x in range(w)] for y in range(h)]
n = 0

@app.route("/fetchData")
def fetch_data():
        print(dataPoints)
	return jsonify(points=dataPoints)

@app.route("/")
def fetch_index():
	return render_template("index.html")

@app.route("/api/pluto", methods=["POST"])
def listen_for_pluto():
	global n
	payload = json.loads(request.get_data())
	if not payload:
		abort(400, "No payload in POST...")

	Signals[n][0] = payload.get("PlutoID")
	Signals[n][1] = payload.get("Band")
	Signals[n][2] = payload.get("Bin")
	Signals[n][3] = payload.get("RSS")
	Signals[n][4] = payload.get("Time")

	if Signals[n][0] == 1:
		Signals[n][5] = 1 #pluto 1 x position
		Signals[n][6] = 2 #pluto 1 y position
	elif Signals[n][0] == 2:
		Signals[n][5] = 3 #pluto 2 x position
		Signals[n][6] = 19 #pluto 2 y position
	elif Signals[n][0] == 3:
		Signals[n][5] = 22 #pluto 3 x position
		Signals[n][6] = 21 #pluto 3 y position
	elif Signals[n][0] == 4:
		Signals[n][5] = 20 #pluto 4 x position
		Signals[n][6] = 4 #pluto 4 y position

	localize(n)
	if n<(w-1):
		n = n + 1
	else:
		n = 0

	return json.dumps({"success":True}), 200, {"ContentType":"application/json"}

def localize(num):
	count = 1
	sameSignal = []
        Solutions = 0

        
	for i in range(0, h):
		keepGoing = 1
		if i != num:
			if Signals[i][0] != Signals[num][0]: #if signals are from diff plutos
				if count > 1: #different from plutos already on list
					for k in range(0, (count-1)):
						if Signals[i][0] == Signals[sameSignal[k]][0]:
							keepGoing = 0
				if keepGoing == 1:
					if Signals[i][1] == Signals[num][1]: #if on the same band
						if Signals[i][2] == Signals[num][2]: #if on the same bin
							if abs(Signals[i][4]-Signals[num][4]) < 6: #if 5s or less between signals
								count = count + 1
								sameSignal.append(i)

                                                                

	if count >= 4:
                sameSignal.append(num)
		a = 2
		ref = 0 #reference node
		Eqs = []
		beqs = []
                xSolutions = []
                ySolutions = []
		while ref < count:
			m=0 #index in equations array
                        p=m #index in sameSignal array
			while m < (count-1):
				Eqs.append([])
				if ref == p:
                                        if ref == (count - 1):
					        p = 0
                                        else:
                                                p = p + 1
                                Piref = 0.1*np.log(10)*(Signals[sameSignal[p]][3]-Signals[sameSignal[ref]][3])
                                riref = np.exp(-(2/a)*Piref)
     			        Eqs.append([])
                                Eqs[m].append(2*Signals[sameSignal[p]][5] - 2*riref*Signals[sameSignal[ref]][5]) #x
                                Eqs[m].append(2*Signals[sameSignal[p]][6] - 2*riref*Signals[sameSignal[ref]][6]) #y
                                Eqs[m].append(riref-1) #R
			        beqs.append(Signals[sameSignal[p]][5]**2 + Signals[sameSignal[p]][6]**2 - riref*(Signals[sameSignal[ref]][5]**2 + Signals[sameSignal[ref]][6]**2)) #b

                                m = m + 1
                                p = p + 1

                                

			A = np.array(Eqs[0:m])
			b = np.array([beqs[0:m]])
                        #print(Signals)
                        #print(A)
                        #print(b)
                        
                        try:
			        x,y,R= np.linalg.lstsq(A, b.T, rcond=None)[0]
			        xSolutions.append(x[0])
                                ySolutions.append(y[0])
                                x = []
                                y = []
                                Solutions = 1
                        except (ValueError):
                                print('no solution, still going')
                                
                        Eqs = []
                        beqs = []
			ref = ref + 1

                if Solutions:
                        Xsum = 0
		        Ysum = 0
		        sizeSol = 0
		        dist = []
                        
		        for s in xSolutions:
			        Xsum = Xsum + s
                        for s in ySolutions:
			        Ysum = Ysum + s
			        sizeSol = sizeSol + 1
 
		        Xmean = Xsum/sizeSol
		        Ymean = Ysum/sizeSol

		        for s in range(0,sizeSol):
			        dist.append(np.sqrt((xSolutions[s]-Xmean)**2+(ySolutions[s]-Ymean)**2))

		        minIndex = np.argmin(dist)

		        #need to add in transmitted power calculation
		        rss_var = 900
                        dataPoints.append({"x":int(xSolutions[minIndex])*75, "y":int(ySolutions[minIndex])*50, "value":rss_var, "radius":150})
                        print('dataPoints:', dataPoints)

if __name__ == "__main__":
	app.run(debug=True)
