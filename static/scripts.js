window.onload = function() {
	function $(id) {
		return document.getElementById(id);
	}

	var legendCanvas = document.createElement("canvas");
	legendCanvas.width = 100;
	legendCanvas.height = 10;

	var legendCtx = legendCanvas.getContext("2d");
	var gradientCfg = {};

	function updateLegend(data) {
		$("min").innerHTML = data.min;
		$("max").innerHTML = data.max;
		if (data.gradient != gradientCfg) {
			gradientCfg = data.gradient;
			var gradient = legendCtx.createLinearGradient(0, 0, 100, 1);
			for (var key in gradientCfg) {
				gradient.addColorStop(key, gradientCfg[key]);
			}
			legendCtx.fillStyle = gradient;
			legendCtx.fillRect(0, 0, 100, 10);
			$("gradient").src = legendCanvas.toDataURL();
		}
	}

	var heatmap = h337.create({
		container: document.getElementById("heatmapContainer"),
		maxOpacity: .5,
		radius: 10,
		blur: .75,
		onExtremaChange: function onExtremaChange(data) {updateLegend(data);}
	});

        window.heatmap = heatmap;


	window.fetchData = function() {
		var xhr = new XMLHttpRequest();
	        xhr.open("GET", "/fetchData");
	        xhr.onload = function() {
		    if (xhr.status === 200) {
				var dataPoints = JSON.parse(xhr.responseText);
				heatmap.setData({
					min: 0,
					max: 1000,
				    data: dataPoints.points
				});
			} else {
				console.log("Request failed. Returned status of " + xhr.status);
			}
		}
		xhr.send();
	}
	fetchData();
	setInterval(function() {
		fetchData()
	}, 1000);
}
